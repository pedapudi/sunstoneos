#!/usr/bin/env python3
"""The pre-push hook refuses each blocked class of added content and passes ordinary code.

Each case pushes from a temporary clone to a temporary bare repository with
core.hooksPath set to tools/git-hooks. Blocked samples are assembled from
separate string pieces so that this file passes the hook's own scan.
"""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ('tools/git-hooks/pre-push', 'checks/pre-push-hook.py', 'checks/harness.py')
HOOKS = ROOT / 'tools/git-hooks'
ENV = {**harness.environment(), 'GIT_CONFIG_GLOBAL': os.devnull, 'GIT_CONFIG_NOSYSTEM': '1',
       'GIT_AUTHOR_NAME': 'Check', 'GIT_AUTHOR_EMAIL': 'check@example.invalid',
       'GIT_COMMITTER_NAME': 'Check', 'GIT_COMMITTER_EMAIL': 'check@example.invalid'}

# Blocked samples: (case name, file name, file text, rule the hook must report).
BLOCKED_CONTENT = [
    ('private key', 'key.txt', '-----BEGIN ' + 'OPENSSH PRIVATE KEY-----\nAAAA\n', 'private key block'),
    ('GitHub classic token', 'a.py', "TOKEN = '" + 'gh' + 'p_' + 'a1B2' * 9 + "'\n", 'GitHub token'),
    ('GitHub OAuth token', 'a.py', "t = '" + 'gh' + 'o_' + 'Z9y8' * 9 + "'\n", 'GitHub token'),
    ('GitHub fine-grained token', 'a.sh', 'export T=' + 'github' + '_pat_' + '11ABCDEFG0' * 5 + '_xyz\n',
     'GitHub fine-grained token'),
    ('AWS access key', 'a.ini', 'aws_access_key_id = ' + 'AK' + 'IA' + 'IOSFODNN7EXAMPLE\n', 'AWS access key ID'),
    ('Slack token', 'a.json', '{"slack": "' + 'xo' + 'xb-' + '1234567890-abcdefABCDEF"}\n', 'Slack token'),
    ('Anthropic key', 'a.env.txt', 'KEY=' + 'sk-' + 'ant-' + 'api03-' + 'Ab1_' * 10 + '\n', 'Anthropic API key'),
    ('OpenAI key', 'b.py', "key = '" + 'sk-' + 'proj-' + 'Qw3rTy' * 8 + "'\n", 'OpenAI API key'),
    ('Google key', 'c.js', "const k = '" + 'AI' + 'za' + 'SyA1b2C3d4E5f6G7h8I9j0KlMnOpQrStUvW' + "';\n",
     'Google API key'),
    ('assistant scratch path', 'notes.md', 'Output under /tmp/' + 'claude-1000/run/log.txt\n', 'session scratch path'),
    ('assistant project path', 'notes.md', 'See ~/.' + 'claude/projects/x/memory.md\n', 'session scratch path'),
]
BLOCKED_FILES = ['.env', 'config/.env.production', 'id_rsa', 'deploy/id_ed25519', 'server.pem', 'credentials.json',
                 '.netrc']
BLOCKED_MESSAGES = [
    ('assistant co-author', 'Add a driver\n\nCo-Authored-By: Claude Opus <noreply@anthropic.com>\n',
     'assistant co-author line'),
    ('other assistant co-author', 'Add a driver\n\nCo-authored-by: GitHub Copilot <copilot@github.com>\n',
     'assistant co-author line'),
    ('generation credit', 'Add a driver\n\nGenerated with [Claude Code](https://claude.com/claude-code)\n',
     'assistant generation credit'),
    ('token in message', 'Rotate ' + 'gh' + 'p_' + 'Q' * 36 + '\n', 'GitHub token'),
]
# Ordinary content that must pass.
PASSING_CONTENT = [
    ('secrets module', 'tool.py', 'import secrets\n\ntoken = secrets.token_hex(16)\nkey = secrets.token_bytes(32)\n'),
    ('driver key and token variables', 'drivers/crypto.c',
     'static uint32_t key[8];\nint token = read_token(dev);\nconst char *api_key_name = "aes-key";\n'),
    ('SHA-256 digests', 'manifest.json',
     '{"sha256": "' + hashlib.sha256(b'x').hexdigest() + '",\n "upper": "' + hashlib.sha256(b'y').hexdigest().upper()
     + '"}\n'),
    ('allowlisted fixture', 'fixture.py',
     "SAMPLE = '" + 'gh' + 'p_' + 'a1B2' * 9 + "'  # pre-push: allow\n"),
    ('kebab-case names', 'cmake/x.cmake', 'set(NAMES disk-image-builder-for-the-a733-board task-sk-names)\n'),
    ('example environment file', '.env.example', 'API_URL=https://example.invalid\n'),
    ('public key file', 'id_rsa.pub', 'ssh-rsa AAAA user@host\n'),
]
PASSING_MESSAGES = [
    'Add a driver\n\nCo-Authored-By: Sunil Pedapudi <sunil@example.invalid>\n',
    'Regenerate tables\n\nGenerated with cmake from the vendor register list.\n',
    'Use the secrets module for the session key token\n',
]


def git(cwd, *args, check=True, stdin=None):
    return subprocess.run(['git', *args], cwd=cwd, env=ENV, capture_output=True, text=True, check=check, input=stdin)


class Repo:
    def __init__(self, base):
        self.origin, self.work = base / 'origin.git', base / 'work'
        git(base, 'init', '-q', '--bare', '-b', 'bringup', str(self.origin))
        git(base, 'init', '-q', '-b', 'bringup', str(self.work))
        git(self.work, 'remote', 'add', 'origin', str(self.origin))
        git(self.work, 'config', 'core.hooksPath', str(HOOKS))
        self.write('README.md', 'base\n')
        self.commit('Add the base')
        self.push('bringup')
        self.branches = 0

    def write(self, name, text):
        path = self.work / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        git(self.work, 'add', '--', name)

    def commit(self, message):
        git(self.work, 'commit', '-q', '--allow-empty', '-F', '-', stdin=message)

    def branch(self):
        self.branches += 1
        name = f'case-{self.branches}'
        git(self.work, 'checkout', '-q', '-b', name, 'bringup')
        return name

    def push(self, branch):
        result = git(self.work, 'push', '-q', 'origin', branch, check=False)
        return result.returncode, result.stderr


def blocked(repo, case, rule, branch, location=None):
    code, err = repo.push(branch)
    assert code != 0, f'{case}: push was accepted'
    assert 'pre-push: refused' in err and rule in err, f'{case}: expected {rule!r}, got:\n{err}'
    if location:
        assert location in err, f'{case}: expected location {location!r}, got:\n{err}'


def passed(repo, case, branch):
    code, err = repo.push(branch)
    assert code == 0, f'{case}: push was refused:\n{err}'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    ENV['SUNSTONE_CHECK_OUTPUT'] = str(args.output / 'hook-evidence')
    temp = tempfile.mkdtemp(prefix='pushes-', dir=args.output)
    repo = Repo(Path(temp))
    for case, name, text, rule in BLOCKED_CONTENT:
        branch = repo.branch()
        repo.write(name, text)
        repo.commit('Add ' + case)
        blocked(repo, case, rule, branch, f'{name}:1 in commit')
    for name in BLOCKED_FILES:
        branch = repo.branch()
        repo.write(name, 'x\n')
        repo.commit('Add ' + name)
        blocked(repo, name, 'credential file added by name', branch, name)
    for case, message, rule in BLOCKED_MESSAGES:
        branch = repo.branch()
        repo.commit(message)
        blocked(repo, case, rule, branch, 'message line')
    for case, name, text in PASSING_CONTENT:
        branch = repo.branch()
        repo.write(name, text)
        repo.commit('Add ' + case)
        passed(repo, case, branch)
    for message in PASSING_MESSAGES:
        branch = repo.branch()
        repo.commit(message)
        passed(repo, message.splitlines()[0], branch)

    token = 'gh' + 'p_' + 'R' * 36
    # A token that a later commit removes is still in the pushed history.
    branch = repo.branch()
    repo.write('later.py', f"T = '{token}'\n")
    repo.commit('Add a token')
    repo.write('later.py', 'T = None\n')
    repo.commit('Remove the token')
    blocked(repo, 'token removed later', 'GitHub token', branch, 'later.py:1')

    # An update to an existing remote branch scans only the new commits.
    branch = repo.branch()
    repo.write('update.c', 'int x;\n')
    repo.commit('Add a file')
    passed(repo, 'first push of a branch', branch)
    repo.write('update.c', 'int x;\n++ b/elsewhere\n' + f"const char *t = \"{token}\";\n")
    repo.commit('Add a token to a pushed branch')
    blocked(repo, 'update of a pushed branch', 'GitHub token', branch, 'update.c:3')

    # Content already on the remote is not scanned again.
    branch = repo.branch()
    repo.write('old.py', f"T = '{token}'\n")
    repo.commit('Add an old token')
    assert git(repo.work, 'push', '-q', '--no-verify', 'origin', branch, check=False).returncode == 0
    repo.write('new.py', 'x = 1\n')
    repo.commit('Add a clean file')
    passed(repo, 'clean commit after a published one', branch)

    # A merge contributes the lines its conflict resolution wrote.
    base = repo.branch()
    repo.write('merge.txt', 'one\n')
    repo.commit('Add the merge file')
    assert git(repo.work, 'push', '-q', '--no-verify', 'origin', base, check=False).returncode == 0
    git(repo.work, 'checkout', '-q', '-b', base + '-side')
    repo.write('merge.txt', 'side\n')
    repo.commit('Change on the side')
    git(repo.work, 'checkout', '-q', base)
    repo.write('merge.txt', 'main\n')
    repo.commit('Change on the main line')
    git(repo.work, 'merge', '-q', base + '-side', check=False)
    repo.write('merge.txt', f'resolved {token}\n')
    repo.commit('Merge the side change')
    blocked(repo, 'merge resolution', 'GitHub token', base, 'merge.txt:1')
    # A fixture in the pushed checkout records its invocation. Hook inputs
    # trigger it; an unrelated driver or document must leave it untouched.
    branch = repo.branch()
    repo.write('checks/pre-push-hook.py', 'from pathlib import Path\n'
               'CHECK_INPUTS = ("fixture-input.txt",)\n'
               'Path("fixture-ran").write_text("ran")\n')
    repo.commit('Add hook fixture')
    code, err = repo.push(branch)
    assert code == 0 and 'hook fixture selected' in err, err
    marker = repo.work / 'fixture-ran'
    assert marker.read_text() == 'ran'
    marker.rename(Path(temp) / 'fixture-first')
    for name in ('drivers/ordinary.c', 'docs/ordinary.md'):
        repo.write(name, 'ordinary\n')
        repo.commit('Change an unrelated input')
        code, err = repo.push(branch)
        assert code == 0 and 'hook fixture selected' not in err, err
        assert not marker.exists()
    for number, name in enumerate(('fixture-input.txt', 'tools/git-hooks/pre-push', 'checks/harness.py')):
        repo.write(name, '# fixture input\n')
        repo.commit('Change a hook fixture input')
        code, err = repo.push(branch)
        assert code == 0 and 'hook fixture selected' in err, err
        assert marker.read_text() == 'ran'
        marker.rename(Path(temp) / ('fixture-input-' + str(number)))
    print(f'PASS: {len(BLOCKED_CONTENT) + len(BLOCKED_FILES) + len(BLOCKED_MESSAGES) + 3} blocked and '
          f'{len(PASSING_CONTENT) + len(PASSING_MESSAGES) + 8} passing pushes')


if __name__ == '__main__':
    main()
