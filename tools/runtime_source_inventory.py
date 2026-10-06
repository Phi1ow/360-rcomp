"""Conservative source inventory for the runtime selected by R-comp's build.

This is source-level evidence, not execution proof. New HLE source files do
not become supported merely by existing: their translation units must appear
in the runtime library and their registration must be reachable from the
production bootstrap roots. Runtime/PS5 checks remain separate requirements.
"""
from pathlib import Path
import re


def without_comments(text):
    pattern = r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/'
    return re.sub(pattern, lambda match: ' ' * len(match[0])
                  if match[0].startswith(('//', '/*')) else match[0], text)


def function_body(text, opening):
    depth = 0
    quote = None
    escape = False
    for offset in range(opening, len(text)):
        character = text[offset]
        if quote:
            if escape:
                escape = False
            elif character == '\\':
                escape = True
            elif character == quote:
                quote = None
            continue
        if character in ('"', "'"):
            quote = character
        elif character == '{':
            depth += 1
        elif character == '}':
            depth -= 1
            if not depth:
                return text[opening:offset + 1]
    raise ValueError('unterminated runtime registration function')


def selected_runtime_sources(root):
    root = Path(root).resolve()
    cmake = re.sub(r'#[^\n]*', '', (root / 'runtime/CMakeLists.txt').read_text())
    files = set()
    for match in re.finditer(r'add_library\(\s*rcomp_runtime(?:_video)?\s+STATIC\b([^)]*)\)',
                             cmake, re.I):
        for token in re.findall(r'[^\s"]+', match[1]):
            if token.endswith(('.cpp', '.c')):
                path = (root / 'runtime' / token).resolve()
                if '${' in token or not path.is_relative_to(root / 'runtime') or not path.is_file():
                    raise ValueError('unsupported/missing runtime source selection: ' + token)
                files.add(path)
    if not files:
        raise ValueError('runtime library has no explicit source inventory')
    return files


def registration_sources(root):
    root = Path(root).resolve()
    texts = {path: without_comments(path.read_text(encoding='utf-8'))
             for path in selected_runtime_sources(root)}
    functions = {}
    definition = re.compile(r'\bStatus\s+(register_(?:xboxkrnl|xam)\w*|'
                            r'register_thread_object_type_variable)\s*\([^;{}]*\)\s*\{')
    for path, text in texts.items():
        for match in definition.finditer(text):
            name = match[1]
            if name in functions:
                raise ValueError('duplicate runtime registration definition: ' + name)
            functions[name] = (path, function_body(text, match.end() - 1))
    roots = ['register_xboxkrnl_hle', 'register_xam_hle', 'register_xboxkrnl_video_hle']
    bootstrap = root / 'app/src/title_runtime.cpp'
    if bootstrap.is_file():
        roots += re.findall(r'\b(register_(?:xboxkrnl|xam)\w*|register_thread_object_type_variable)\s*\(',
                            without_comments(bootstrap.read_text(encoding='utf-8')))
    pending, reached, active = list(roots), set(), set()
    while pending:
        name = pending.pop()
        if name in reached:
            continue
        reached.add(name)
        if name not in functions:
            raise ValueError('bootstrap registration is missing from runtime build: ' + name)
        path, body = functions[name]
        active.add(path)
        calls = re.findall(r'\b(register_(?:xboxkrnl|xam)\w*|register_thread_object_type_variable)\s*\(', body)
        pending.extend(call for call in calls if call != name)
    return {path: texts[path] for path in active}


def source_imports(root):
    functions, variables = set(), set()
    sources = registration_sources(root)
    for path, text in sources.items():
        module = ('xam.xex' if path.name.startswith('hle_xam') else
                  'xboxkrnl.exe' if path.name.startswith('hle_xboxkrnl') else None)
        if module:
            for ordinal in re.findall(r'\{\s*(0x[0-9a-fA-F]+)\s*,\s*"\w+"\s*,\s*&', text):
                functions.add((module, int(ordinal, 16)))
            for block in re.findall(r'\bvars\[\]\s*=\s*\{(.*?)\};', text, re.S):
                for ordinal in re.findall(r'\{\s*(0x[0-9a-fA-F]+)\s*,', block):
                    variables.add((module, int(ordinal, 16)))
        # Direct registrations such as the opaque ExThreadObjectType live in
        # a non-HLE implementation unit reached from TitleRuntime bootstrap.
        for name, ordinal in re.findall(
                r'\bregister_variable_import\(\s*(kModuleXboxkrnl|kModuleXam)\s*,\s*'
                r'(0x[0-9a-fA-F]+)\s*,', text):
            variables.add(('xboxkrnl.exe' if name == 'kModuleXboxkrnl' else 'xam.xex', int(ordinal, 16)))
    return functions, variables, sorted(path.relative_to(Path(root).resolve()).as_posix() for path in sources)
