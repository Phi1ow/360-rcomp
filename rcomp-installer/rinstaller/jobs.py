"""Background jobs with a live log. One job may hold the console at a time."""
import itertools
import threading
import time
import traceback

STATUSES = ('PASS', 'FAIL', 'BLOCKED', 'NOT TESTED')


class Cancelled(Exception):
    pass


class Blocked(Exception):
    """A precondition is missing: the step could not run (status BLOCKED, not FAIL)."""


class Failed(Exception):
    """The step ran and did not do what it must (status FAIL)."""


class Job:
    _ids = itertools.count(1)

    def __init__(self, kind, title, log_dir=None):
        self.id = next(self._ids)
        self.kind = kind
        self.title = title
        self.lines = []          # (seq, time, level, text)
        self.status = 'RUNNING'  # then one of STATUSES
        self.summary = ''
        self.result = {}
        self.started = time.time()
        self.ended = None
        self.cancel_event = threading.Event()
        self.cond = threading.Condition()
        self.logfile = None
        if log_dir:
            log_dir.mkdir(parents=True, exist_ok=True)
            self.logfile = log_dir / f'{time.strftime("%Y%m%d-%H%M%S")}-{self.id}-{kind}.log'

    def log(self, level, text):
        """level: STEP, INFO, OUT (subprocess output), WARN, PASS, FAIL, BLOCKED or NOT TESTED."""
        with self.cond:
            stamp = time.time()
            new = str(text).splitlines() or ['']
            for line in new:
                self.lines.append((len(self.lines), stamp, level, line))
            self.cond.notify_all()
            if self.logfile:
                try:
                    with open(self.logfile, 'a', encoding='utf-8') as f:
                        t = time.strftime('%H:%M:%S', time.localtime(stamp))
                        f.writelines(f'{t} {level:<10} {line}\n' for line in new)
                except OSError:
                    pass

    def check_cancel(self):
        if self.cancel_event.is_set():
            raise Cancelled('cancelled by the user')

    def finish(self, status, summary, result=None):
        assert status in STATUSES, status
        with self.cond:
            self.status = status
            self.summary = summary
            if result is not None:
                self.result = result  # a failed job keeps what it recorded so far
            self.ended = time.time()
            self.cond.notify_all()

    def view(self, since=0):
        with self.cond:
            return {'id': self.id, 'kind': self.kind, 'title': self.title, 'status': self.status,
                    'summary': self.summary, 'result': self.result, 'started': self.started, 'ended': self.ended,
                    'lines': [{'seq': s, 't': t, 'level': lv, 'text': tx} for s, t, lv, tx in self.lines[since:]]}

    def wait(self, since, timeout):
        """Block until there are lines after `since` or the job ended."""
        with self.cond:
            self.cond.wait_for(lambda: len(self.lines) > since or self.status != 'RUNNING', timeout)


class JobManager:
    def __init__(self, log_dir=None):
        self.log_dir = log_dir
        self.jobs = {}
        self.lock = threading.Lock()
        self.console_lock = threading.Lock()

    def start(self, kind, title, fn, uses_console=False):
        """Run fn(job) in a thread. fn returns (status, summary, result)."""
        job = Job(kind, title, self.log_dir)
        with self.lock:
            self.jobs[job.id] = job

        def run():
            held = False
            try:
                if uses_console:
                    if not self.console_lock.acquire(blocking=False):
                        job.log('BLOCKED', 'another job is using the console; wait for it to finish')
                        job.finish('BLOCKED', 'console busy with another job')
                        return
                    held = True
                status, summary, result = fn(job)
                job.log(status, summary)
                job.finish(status, summary, result)
            except Cancelled as e:
                job.log('FAIL', str(e))
                job.finish('FAIL', str(e))
            except Blocked as e:
                job.log('BLOCKED', str(e))
                job.finish('BLOCKED', str(e))
            except Failed as e:
                job.log('FAIL', str(e))
                job.finish('FAIL', str(e))
            except Exception as e:  # an unexpected error is a failure, with its trace in the log
                job.log('FAIL', f'{type(e).__name__}: {e}')
                job.log('INFO', traceback.format_exc())
                job.finish('FAIL', f'{type(e).__name__}: {e}')
            finally:
                if held:
                    self.console_lock.release()

        threading.Thread(target=run, name=f'job-{job.id}', daemon=True).start()
        return job

    def get(self, job_id):
        return self.jobs.get(job_id)

    def list(self):
        with self.lock:
            return [{'id': j.id, 'kind': j.kind, 'title': j.title, 'status': j.status, 'summary': j.summary,
                     'started': j.started, 'ended': j.ended} for j in sorted(self.jobs.values(), key=lambda j: -j.id)]
