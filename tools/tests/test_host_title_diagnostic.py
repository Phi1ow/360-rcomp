import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from host_title_diagnostic import validate_function_only_blockers


class DiagnosticAdmissionTests(unittest.TestCase):
    def setUp(self):
        self.report = {
            'supported_for_link': False,
            'imports': {'xam.xex': {'functions_missing': ['XamTaskSchedule'],
                                   'variables_missing': [], 'unknown_ordinals': []}},
            'blocking_reasons': ['unsupported imports in xam.xex: XamTaskSchedule']}
        self.errors = ['inventory did not explicitly permit linking',
                       'inventory blockers are missing or nonempty',
                       'xam.xex: functions_missing is missing or nonempty']

    def test_known_function_failure_is_diagnostic_only(self):
        before = copy.deepcopy(self.report)
        validate_function_only_blockers(self.report, self.errors)
        self.assertEqual(self.report, before)
        self.assertIs(self.report['supported_for_link'], False)

    def test_non_function_integrity_failures_cannot_be_admitted(self):
        for error in ('artifact hashes/sizes disagree with inventory or are missing',
                      'entry point is absent from the generated function table',
                      'recompiler diagnostics are missing or nonempty: warnings',
                      'XEX static TLS template exceeds data_size',
                      'successful recompilation is required, not an inventory-only run',
                      'runtime build/registration sources changed after inventory'):
            with self.subTest(error=error), self.assertRaises(ValueError):
                validate_function_only_blockers(self.report, self.errors + [error])

    def test_unbound_variable_is_not_a_missing_function(self):
        for field in ('variables_missing', 'unknown_ordinals'):
            report = copy.deepcopy(self.report)
            report['imports']['xam.xex'][field] = ['unresolved']
            with self.subTest(field=field), self.assertRaises(ValueError):
                validate_function_only_blockers(report, self.errors)

    def test_false_or_malformed_support_claims_are_rejected(self):
        report = copy.deepcopy(self.report)
        report['supported_for_link'] = True
        with self.assertRaises(ValueError):
            validate_function_only_blockers(report, self.errors)
        report = copy.deepcopy(self.report)
        report['blocking_reasons'].append('unimplemented instructions: mfsprg')
        with self.assertRaises(ValueError):
            validate_function_only_blockers(report, self.errors)
        report = copy.deepcopy(self.report)
        report['imports']['xam.xex']['functions_missing'] = None
        with self.assertRaises(ValueError):
            validate_function_only_blockers(report, self.errors)


if __name__ == '__main__':
    unittest.main()
