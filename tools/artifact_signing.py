r"""Sign and verify the release binaries with Azure Artifact Signing.

    python tools\artifact_signing.py check
    python tools\artifact_signing.py fetch [--archive package.nupkg] [--dry-run]
    python tools\artifact_signing.py sign FILE... [--dry-run]
    python tools\artifact_signing.py verify FILE...
    python tools\artifact_signing.py --self-test

build.bat calls `sign` on every shipped binary when EDUP_SIGN is set (d3d11.dll before it
is copied to edvr_openxr_graphics.dll, edvr_openxr_runtime.dll before it becomes
openvr_api.dll, then ed-up-launcher.exe), so package.bat zips and hashes the signed bytes.
Unset, nothing here runs and a build is what it always was.

What it needs: signtool.exe from a Windows SDK 10.0.22621 or newer, the .NET runtime the client's
runtimeconfig names (10 for the pinned 1.0.151; `check` reads it), the
Microsoft.ArtifactSigning.Client package (`fetch` puts its x64 files under
third_party\artifact_signing, verified against a pinned NuGet SHA-512), and an Azure
credential for an account holding "Artifact Signing Certificate Profile Signer" on the
certificate profile (`az login` is enough; the client reads it through DefaultAzureCredential).
`check` says which of these is missing.

The decisions, kept from the first signing commit (ed71f331):
  * A build asked to sign that cannot, fails. An unsigned binary looks the same as a signed
    one on the machine that made it and different only on the machine that downloads it.
  * Every signature is verified after it is made: `signtool verify /pa`, the policy Windows
    applies to a downloaded program, and the signer's name and a timestamp.
  * Always timestamped. The service's certificates last about three days, so the timestamp
    is what keeps a signature valid afterwards. Transient failures (the timestamp server, the
    network) are retried; a refused credential is not, because it will not change.
"""
import argparse
import base64
import hashlib
import io
import json
import os
import re
import subprocess
import sys
import tempfile
import time
import zipfile
from pathlib import Path

import edup_env

ROOT = Path(__file__).resolve().parents[1]

PACKAGE_ID = 'Microsoft.ArtifactSigning.Client'
VERSION = '1.0.151'
URL = ('https://api.nuget.org/v3-flatcontainer/microsoft.artifactsigning.client/'
       '1.0.151/microsoft.artifactsigning.client.1.0.151.nupkg')
PACKAGE_SIZE = 14740357
# NuGet's own catalog entry for 1.0.151 (packageHash, base64 SHA-512).
PACKAGE_SHA512 = 'BvPApD/Bxbzj2uf8qBYJ0wPoZYvAySHfrsIMZ2JkMQXy7NdTbQ81Gs71A0n/tOYOIWk3xpKY3YVZ1VtH0jcZhQ=='
MEMBER_PREFIX = 'bin/x64/'
DLIB = 'Azure.CodeSigning.Dlib.dll'
DEST = ROOT / 'third_party' / 'artifact_signing'
MARKER = 'PINNED'

# Not secret: these name the account and profile, which is what a signature is made under.
ENDPOINT = 'https://eus.codesigning.azure.net'
ACCOUNT = 'ed-up-signing'
PROFILE = 'ed-up-release'
SUBJECT = 'CharacterEcho LLC'
TIMESTAMP_URL = 'http://timestamp.acs.microsoft.com'
MIN_SDK = (10, 0, 22621, 0)
ATTEMPTS = 3
RETRY_SECONDS = (5, 15)
TIMEOUT = 600

TRANSIENT = ('timestamp', 'timed out', 'timeout', 'could not be reached', 'connection', 'network',
             'temporarily', ' 502', ' 503', ' 504', 'try again')
CREDENTIAL = ('defaultazurecredential', 'credentialunavailable', 'authenticationfailed', 'unauthorized',
              ' 401', ' 403', 'forbidden', 'az login', 'no valid credential')


class SigningError(Exception):
    pass


def setting(name, default, env=None):
    value = edup_env.get('EDUP_SIGN_' + name, environ=env)
    return value if value else default


def sha512_b64(data):
    return base64.b64encode(hashlib.sha512(data).digest()).decode('ascii')


def safe_member(name):
    """A package path under bin/x64/ that stays under the destination, or None."""
    if not name.startswith(MEMBER_PREFIX) or name.endswith('/'):
        return None
    relative = name[len(MEMBER_PREFIX):]
    parts = relative.split('/')
    if not relative or '\\' in relative or ':' in relative or any(p in ('', '.', '..') for p in parts):
        raise ValueError('unsafe path in the signing client package: ' + name)
    return relative


def pinned_members(package, size=PACKAGE_SIZE, sha512=PACKAGE_SHA512):
    """{relative path: bytes} of the x64 client files, only from a package that matches the pin."""
    if len(package) != size or sha512_b64(package) != sha512:
        raise ValueError('signing client package differs from the pinned NuGet release ' + VERSION)
    files = {}
    with zipfile.ZipFile(io.BytesIO(package)) as archive:
        for info in archive.infolist():
            relative = safe_member(info.filename)
            if relative is not None:
                files[relative] = archive.read(info)
    if DLIB not in files:
        raise ValueError('signing client package has no ' + MEMBER_PREFIX + DLIB)
    return files


def marker_text():
    return PACKAGE_ID + ' ' + VERSION + ' ' + PACKAGE_SHA512 + '\n'


def verify_fetched(dest):
    dest = Path(dest)
    marker = dest / MARKER
    if not marker.is_file() or marker.read_text(encoding='ascii', errors='replace') != marker_text():
        raise ValueError(str(dest) + ' is not the pinned signing client; run: python tools\\artifact_signing.py fetch')
    if not (dest / DLIB).is_file():
        raise ValueError(str(dest / DLIB) + ' is missing; run: python tools\\artifact_signing.py fetch')


def write_atomic(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(content)
    try:
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def fetch(dest, archive=None, dry_run=False, opener=None):
    dest = Path(dest)
    if dry_run:
        print('Would fetch ' + (str(archive) if archive else URL) + ' (' + str(PACKAGE_SIZE) +
              ' bytes), verify its SHA-512, and place ' + MEMBER_PREFIX + '* in ' + str(dest))
        return 0
    if archive:
        package = Path(archive).read_bytes()
    else:
        import urllib.request
        with (opener or urllib.request.urlopen)(URL, timeout=120) as response:
            package = response.read(PACKAGE_SIZE + 1)
    files = pinned_members(package)
    for relative, content in files.items():
        write_atomic(dest / relative, content)
    write_atomic(dest / MARKER, marker_text().encode('ascii'))
    verify_fetched(dest)
    print('Prepared %s %s (%d files) in %s' % (PACKAGE_ID, VERSION, len(files), dest))
    return 0


def sdk_version(name):
    match = re.fullmatch(r'(\d+)\.(\d+)\.(\d+)\.(\d+)', name)
    return tuple(int(part) for part in match.groups()) if match else None


def find_signtool(kits=None, env=None):
    """EDUP_SIGNTOOL, else the newest x64 signtool.exe of a Windows SDK >= MIN_SDK."""
    environment = os.environ if env is None else env
    override = edup_env.get('EDUP_SIGNTOOL', environ=environment)
    if override:
        if not Path(override).is_file():
            raise SigningError('EDUP_SIGNTOOL names no file: ' + override)
        return Path(override)
    if kits is None:
        base = environment.get('ProgramFiles(x86)') or r'C:\Program Files (x86)'
        kits = Path(base) / 'Windows Kits' / '10' / 'bin'
    kits = Path(kits)
    found = []
    if kits.is_dir():
        for entry in kits.iterdir():
            version = sdk_version(entry.name)
            candidate = entry / 'x64' / 'signtool.exe'
            if version and version >= MIN_SDK and candidate.is_file():
                found.append((version, candidate))
    if not found:
        raise SigningError('no signtool.exe from a Windows SDK 10.0.22621 or newer under %s; install the Windows '
                           'SDK, or point EDUP_SIGNTOOL at one' % kits)
    return max(found)[1]


def required_dotnet(client_dir):
    """The Microsoft.NETCore.App major version the signing client's runtimeconfig asks for, or None.

    1.0.151 wants 10 although Microsoft's setup page still says 8; reading it from the client
    keeps `check` and `sign` right when the pin moves."""
    path = Path(client_dir) / (Path(DLIB).stem + '.runtimeconfig.json')
    try:
        framework = json.loads(path.read_text(encoding='utf-8'))['runtimeOptions']['framework']
        if framework['name'] == 'Microsoft.NETCore.App':
            return int(framework['version'].split('.')[0])
    except (OSError, ValueError, KeyError, TypeError):
        pass
    return None


def dotnet_runtime_at_least(major, runner=subprocess.run):
    """True when `dotnet --list-runtimes` lists Microsoft.NETCore.App `major` or newer (the client rolls forward)."""
    try:
        done = runner(['dotnet', '--list-runtimes'], capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.SubprocessError):
        return False
    if done.returncode != 0:
        return False
    return any(int(m.group(1)) >= major for m in re.finditer(r'^Microsoft\.NETCore\.App (\d+)\.', done.stdout, re.M))


def runtime_problem(major):
    return ('the signing client needs the .NET %d runtime (Microsoft.NETCore.App %d), which `dotnet --list-runtimes` '
            'does not list; install it: winget install Microsoft.DotNet.Runtime.%d' % (major, major, major))


def metadata(env=None):
    return {'Endpoint': setting('ENDPOINT', ENDPOINT, env),
            'CodeSigningAccountName': setting('ACCOUNT', ACCOUNT, env),
            'CertificateProfileName': setting('PROFILE', PROFILE, env)}


def sign_command(signtool, dlib, meta_path, files):
    return ([str(signtool), 'sign', '/fd', 'SHA256', '/tr', TIMESTAMP_URL, '/td', 'SHA256',
             '/dlib', str(dlib), '/dmdf', str(meta_path)] + [str(f) for f in files])


def default_runner(command, env=None):
    return subprocess.run(command, capture_output=True, text=True, errors='replace', timeout=TIMEOUT, env=env)


def classify(output):
    lowered = ' ' + output.lower()
    if any(word in lowered for word in CREDENTIAL):
        return 'credential'
    if any(word in lowered for word in TRANSIENT):
        return 'transient'
    return 'fatal'


def tail(text, lines=25):
    return '\n'.join(text.strip().splitlines()[-lines:])


def sign(files, dlib=None, signtool=None, dry_run=False, runner=default_runner, sleep=time.sleep, env=None):
    files = [Path(f) for f in files]
    if not files:
        raise SigningError('nothing to sign')
    for file in files:
        if not file.is_file():
            raise SigningError('cannot sign ' + str(file) + ': no such file')
    dlib = Path(dlib) if dlib else DEST / DLIB
    meta = metadata(env)
    if dry_run:
        print('Would sign %d file(s) with signtool under %s / %s / %s, timestamp %s:' % (
            len(files), meta['Endpoint'], meta['CodeSigningAccountName'], meta['CertificateProfileName'], TIMESTAMP_URL))
        for file in files:
            print('  ' + str(file))
        return 0
    if os.name != 'nt' and runner is default_runner:
        raise SigningError('signing runs on Windows only')
    if not dlib.is_file():
        raise SigningError(str(dlib) + ' is missing; run: python tools\\artifact_signing.py fetch')
    signtool = Path(signtool) if signtool else find_signtool(env=env)
    major = required_dotnet(dlib.parent)
    if major and runner is default_runner and not dotnet_runtime_at_least(major):
        # Without it signtool exits 3 and prints nothing at all.
        raise SigningError(runtime_problem(major))
    with tempfile.TemporaryDirectory(prefix='edvr-sign-') as scratch:
        meta_path = Path(scratch) / 'metadata.json'
        meta_path.write_text(json.dumps(meta), encoding='ascii')
        command = sign_command(signtool, dlib, meta_path, files)
        for attempt in range(1, ATTEMPTS + 1):
            done = runner(command)
            output = (done.stdout or '') + (done.stderr or '')
            if done.returncode == 0:
                break
            kind = classify(output)
            if kind == 'credential':
                raise SigningError('Azure refused the signing request, so no usable credential was found. Run '
                                   '`az login` as an account with the "Artifact Signing Certificate Profile Signer" '
                                   'role on %s.\n%s' % (meta['CertificateProfileName'], tail(output)))
            if kind == 'fatal' or attempt == ATTEMPTS:
                raise SigningError('signtool failed (exit %d, attempt %d of %d):\n%s' % (
                    done.returncode, attempt, ATTEMPTS, tail(output)))
            print('artifact_signing: transient failure, retrying (attempt %d of %d)' % (attempt, ATTEMPTS))
            sleep(RETRY_SECONDS[min(attempt - 1, len(RETRY_SECONDS) - 1)])
    verify(files, signtool=signtool, runner=runner, env=env)
    for file in files:
        print('artifact_signing: signed and verified ' + str(file))
    return 0


SIGNATURE_QUERY = ("$s = Get-AuthenticodeSignature -LiteralPath $env:EDUP_SIGN_FILE; "
                   "[pscustomobject]@{ Status = [string]$s.Status; "
                   "Subject = if ($s.SignerCertificate) { $s.SignerCertificate.Subject } else { '' }; "
                   "Timestamped = [bool]$s.TimeStamperCertificate } | ConvertTo-Json -Compress")


def signature_facts(file, runner, env=None):
    environment = dict(os.environ if env is None else env)
    environment['EDUP_SIGN_FILE'] = str(file)
    done = runner(['powershell', '-NoProfile', '-NonInteractive', '-Command', SIGNATURE_QUERY], env=environment)
    if done.returncode != 0:
        raise SigningError('could not read the signature of %s: %s' % (file, tail((done.stdout or '') + (done.stderr or ''))))
    try:
        return json.loads((done.stdout or '').strip().splitlines()[-1])
    except (ValueError, IndexError):
        raise SigningError('unreadable signature report for %s: %r' % (file, done.stdout))


def verify(files, signtool=None, runner=default_runner, env=None):
    files = [Path(f) for f in files]
    if not files:
        raise SigningError('nothing to verify')
    signtool = Path(signtool) if signtool else find_signtool(env=env)
    subject = setting('SUBJECT', SUBJECT, env)
    for file in files:
        if not file.is_file():
            raise SigningError('cannot verify ' + str(file) + ': no such file')
        done = runner([str(signtool), 'verify', '/pa', '/q', str(file)])
        if done.returncode != 0:
            raise SigningError('%s does not satisfy the Authenticode policy Windows applies to a downloaded '
                               'program (signtool verify /pa):\n%s' % (file, tail((done.stdout or '') + (done.stderr or ''))))
        facts = signature_facts(file, runner, env)
        if facts.get('Status') != 'Valid':
            raise SigningError('%s: signature status is %s, not Valid' % (file, facts.get('Status')))
        if not facts.get('Timestamped'):
            raise SigningError('%s is signed but not timestamped: it would stop verifying when the certificate expires' % file)
        if ('O=' + subject) not in (facts.get('Subject') or ''):
            raise SigningError('%s is signed by %r, not by %s' % (file, facts.get('Subject'), subject))
    return 0


def check():
    problems = 0

    def report(label, ok, detail):
        nonlocal problems
        print('%-22s %s  %s' % (label, 'ok     ' if ok else 'MISSING', detail))
        problems += 0 if ok else 1

    try:
        report('signtool.exe', True, str(find_signtool()))
    except SigningError as error:
        report('signtool.exe', False, str(error))
    try:
        verify_fetched(DEST)
        report('signing client', True, str(DEST / DLIB))
        major = required_dotnet(DEST)
        if major:
            report('.NET %d runtime' % major, dotnet_runtime_at_least(major),
                   '`dotnet --list-runtimes` lists Microsoft.NETCore.App %d or newer (winget install '
                   'Microsoft.DotNet.Runtime.%d)' % (major, major))
    except ValueError as error:
        report('signing client', False, str(error))
    meta = metadata()
    print('%-22s %s / %s / %s' % ('signing under', meta['Endpoint'], meta['CodeSigningAccountName'], meta['CertificateProfileName']))
    print('The Azure credential is only proved by signing: run `az login` as an account holding "Artifact Signing '
          'Certificate Profile Signer".')
    return 3 if problems else 0


def self_test():
    checks = 0

    def package(members):
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, 'w') as archive:
            for name, content in members.items():
                archive.writestr(name, content)
        return buffer.getvalue()

    class Done:
        def __init__(self, code=0, out='', err=''):
            self.returncode, self.stdout, self.stderr = code, out, err

    good = package({'bin/x64/' + DLIB: b'dlib', 'bin/x64/Azure.Core.dll': b'core', 'bin/x64/sub/runtime.dll': b'rt',
                    'bin/x86/' + DLIB: b'x86 never read', 'docs/README.md': b'not a client file'})
    files = pinned_members(good, len(good), sha512_b64(good))
    assert sorted(files) == sorted(['Azure.Core.dll', DLIB, 'sub/runtime.dll']) and files[DLIB] == b'dlib'
    checks += 1
    for size, digest in ((len(good) + 1, sha512_b64(good)), (len(good), sha512_b64(b'other'))):
        try:
            pinned_members(good, size, digest)
            raise AssertionError('a package that differs from the pin was accepted')
        except ValueError:
            checks += 1
    for bad in ('bin/x64/../escape.dll', 'bin/x64//double.dll', 'bin/x64/C:evil.dll'):
        evil = package({'bin/x64/' + DLIB: b'd', bad: b'x'})
        try:
            pinned_members(evil, len(evil), sha512_b64(evil))
            raise AssertionError('unsafe path accepted: ' + bad)
        except ValueError:
            checks += 1
    try:
        # zipfile turns a backslash into a slash on Windows, so ask the path check directly.
        safe_member('bin/x64/a\\b.dll')
        raise AssertionError('a backslash path was accepted')
    except ValueError:
        checks += 1
    nodlib = package({'bin/x64/Azure.Core.dll': b'c'})
    try:
        pinned_members(nodlib, len(nodlib), sha512_b64(nodlib))
        raise AssertionError('a package without the dlib was accepted')
    except ValueError:
        checks += 1

    with tempfile.TemporaryDirectory(prefix='edvr-artifact-signing-') as scratch:
        scratch = Path(scratch)
        dest = scratch / 'absent'
        assert fetch(dest, dry_run=True) == 0 and not dest.exists()
        checks += 1
        try:
            verify_fetched(dest)
            raise AssertionError('a missing client was accepted')
        except ValueError:
            checks += 1
        # fetch() checks against the real pin, so exercise its writing half with the synthetic package's own pin.
        for relative, content in files.items():
            write_atomic(dest / relative, content)
        write_atomic(dest / MARKER, marker_text().encode('ascii'))
        verify_fetched(dest)
        assert (dest / 'sub' / 'runtime.dll').read_bytes() == b'rt' and not (scratch / 'escape.dll').exists()
        checks += 1
        (dest / MARKER).write_text('something else\n')
        try:
            verify_fetched(dest)
            raise AssertionError('a foreign marker was accepted')
        except ValueError:
            checks += 1
        try:
            fetch(scratch / 'wrong', archive=scratch / 'missing.nupkg')
            raise AssertionError('a missing archive was accepted')
        except OSError:
            checks += 1
        (scratch / 'wrong.nupkg').write_bytes(good)
        try:
            fetch(scratch / 'wrong', archive=scratch / 'wrong.nupkg')
            raise AssertionError('a package that is not the pinned release was installed')
        except ValueError:
            assert not (scratch / 'wrong').exists()
            checks += 1

        # signtool discovery over a fake Windows Kits tree.
        kits = scratch / 'kits'
        for name in ('10.0.20348.0', '10.0.22621.0', '10.0.26100.0', 'x86', '10.0.bad'):
            (kits / name / 'x64').mkdir(parents=True)
            (kits / name / 'x64' / 'signtool.exe').write_bytes(b'MZ')
        assert find_signtool(kits, {}).parent.parent.name == '10.0.26100.0'
        checks += 1
        (kits / '10.0.26100.0' / 'x64' / 'signtool.exe').unlink()
        assert find_signtool(kits, {}).parent.parent.name == '10.0.22621.0'
        checks += 1
        (kits / '10.0.22621.0' / 'x64' / 'signtool.exe').unlink()
        try:
            find_signtool(kits, {})
            raise AssertionError('a Windows SDK older than 10.0.22621 was accepted')
        except SigningError:
            checks += 1
        explicit = scratch / 'mine.exe'
        explicit.write_bytes(b'MZ')
        assert find_signtool(kits, {'EDUP_SIGNTOOL': str(explicit)}) == explicit
        checks += 1

        # The settings and the command line.
        assert metadata({}) == {'Endpoint': ENDPOINT, 'CodeSigningAccountName': ACCOUNT, 'CertificateProfileName': PROFILE}
        assert metadata({'EDUP_SIGN_PROFILE': 'other'})['CertificateProfileName'] == 'other'
        checks += 1
        command = sign_command('signtool.exe', 'dlib.dll', 'meta.json', ['a.dll', 'b.exe'])
        assert command[1] == 'sign' and command[-2:] == ['a.dll', 'b.exe']
        for flag, value in (('/fd', 'SHA256'), ('/td', 'SHA256'), ('/tr', TIMESTAMP_URL), ('/dlib', 'dlib.dll'), ('/dmdf', 'meta.json')):
            assert command[command.index(flag) + 1] == value
        checks += 1

        # sign() against a fake signtool and a fake PowerShell.
        dll = scratch / 'a.dll'
        dll.write_bytes(b'MZ')
        client = scratch / 'client'
        client.mkdir()
        (client / DLIB).write_bytes(b'd')
        subject = {'Status': 'Valid', 'Subject': 'CN=CharacterEcho LLC, O=CharacterEcho LLC, L=Denver, S=Colorado, C=US',
                   'Timestamped': True}
        calls, naps = [], []

        def make_runner(sign_results, facts):
            results = list(sign_results)

            def runner(command, env=None):
                calls.append(command)
                if command[0] == 'powershell':
                    return Done(0, json.dumps(facts))
                if command[1] == 'sign':
                    return results.pop(0) if results else Done(0)
                return Done(0)
            return runner

        def run(sign_results, facts=subject, expect=None, **extra):
            del calls[:], naps[:]
            try:
                sign([dll], dlib=client / DLIB, signtool='signtool.exe', runner=make_runner(sign_results, facts),
                     sleep=naps.append, env={}, **extra)
            except SigningError as error:
                if expect is None:
                    raise
                assert expect in str(error), str(error)
                return str(error)
            assert expect is None, 'expected a SigningError containing ' + expect

        run([])
        assert [c[1] for c in calls if c[0] != 'powershell'] == ['sign', 'verify'] and not naps
        checks += 1
        run([Done(1, 'The specified timestamp server either could not be reached')])
        assert [c[1] for c in calls if c[0] == 'signtool.exe'] == ['sign', 'sign', 'verify'] and naps == [RETRY_SECONDS[0]]
        checks += 1
        run([Done(1, 'timed out')] * ATTEMPTS, expect='attempt 3 of 3')
        assert len([c for c in calls if c[1] == 'sign']) == ATTEMPTS and naps == list(RETRY_SECONDS)
        checks += 1
        message = run([Done(1, 'Azure.Identity.CredentialUnavailableException: DefaultAzureCredential failed')], expect='az login')
        assert len([c for c in calls if c[1] == 'sign']) == 1 and not naps and 'Certificate Profile Signer' in message
        checks += 1
        run([Done(2, 'Number of errors: 1')], expect='attempt 1 of 3')
        assert len([c for c in calls if c[1] == 'sign']) == 1 and not naps
        checks += 1
        run([], facts=dict(subject, Status='UnknownError'), expect='UnknownError')
        run([], facts=dict(subject, Timestamped=False), expect='not timestamped')
        run([], facts=dict(subject, Subject='CN=Someone Else'), expect='not by CharacterEcho LLC')
        checks += 3

        # --dry-run runs nothing and writes nothing.
        del calls[:]
        before = sorted(p.name for p in scratch.iterdir())
        assert sign([dll], dry_run=True, runner=make_runner([], subject), env={}) == 0
        assert not calls and sorted(p.name for p in scratch.iterdir()) == before
        checks += 1
        for bad in ([], [scratch / 'missing.dll']):
            try:
                sign(bad, dlib=client / DLIB, signtool='signtool.exe', runner=make_runner([], subject), env={})
                raise AssertionError('signing nothing, or a missing file, was accepted')
            except SigningError:
                checks += 1
        try:
            sign([dll], dlib=scratch / 'no-dlib.dll', signtool='signtool.exe', runner=make_runner([], subject), env={})
            raise AssertionError('signing without the client was accepted')
        except SigningError as error:
            assert 'fetch' in str(error)
            checks += 1

    with tempfile.TemporaryDirectory(prefix='edvr-artifact-signing-rc-') as scratch:
        config = Path(scratch) / (Path(DLIB).stem + '.runtimeconfig.json')
        assert required_dotnet(scratch) is None
        config.write_text('{"runtimeOptions":{"framework":{"name":"Microsoft.NETCore.App","version":"10.0.0"}}}')
        assert required_dotnet(scratch) == 10
        config.write_text('{"runtimeOptions":{"framework":{"name":"Other","version":"3.0.0"}}}')
        assert required_dotnet(scratch) is None
        config.write_text('not json')
        assert required_dotnet(scratch) is None
    listing = 'Microsoft.NETCore.App 6.0.23 [x]\nMicrosoft.NETCore.App 8.0.7 [x]\n'
    assert dotnet_runtime_at_least(8, lambda *a, **k: Done(0, listing))
    assert not dotnet_runtime_at_least(10, lambda *a, **k: Done(0, listing))
    assert dotnet_runtime_at_least(10, lambda *a, **k: Done(0, listing + 'Microsoft.NETCore.App 10.0.15 [x]\n'))
    assert dotnet_runtime_at_least(10, lambda *a, **k: Done(0, listing + 'Microsoft.NETCore.App 11.0.1 [x]\n'))
    assert not dotnet_runtime_at_least(8, lambda *a, **k: Done(1, listing))
    assert 'Microsoft.DotNet.Runtime.10' in runtime_problem(10)
    checks += 1
    assert classify('The specified timestamp server could not be reached') == 'transient'
    assert classify('Unauthorized 403') == 'credential' and classify('Number of errors: 1') == 'fatal'
    checks += 1
    print('artifact_signing: %d checks passed (offline)' % checks)
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--self-test', action='store_true')
    commands = parser.add_subparsers(dest='command')
    commands.add_parser('check', help='say which signing prerequisites are missing')
    fetch_parser = commands.add_parser('fetch', help='fetch the pinned signing client into third_party\\artifact_signing')
    fetch_parser.add_argument('--archive', type=Path, help='use this .nupkg instead of downloading it')
    fetch_parser.add_argument('--dest', type=Path, default=DEST)
    fetch_parser.add_argument('--dry-run', action='store_true')
    sign_parser = commands.add_parser('sign', help='sign and verify files')
    sign_parser.add_argument('files', nargs='+', type=Path)
    sign_parser.add_argument('--dry-run', action='store_true')
    verify_parser = commands.add_parser('verify', help='verify files are signed by this project')
    verify_parser.add_argument('files', nargs='+', type=Path)
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.command == 'check':
        return check()
    if args.command == 'fetch':
        return fetch(args.dest, archive=args.archive, dry_run=args.dry_run)
    if args.command == 'sign':
        return sign(args.files, dry_run=args.dry_run)
    if args.command == 'verify':
        verify(args.files)
        for file in args.files:
            print('artifact_signing: verified ' + str(file))
        return 0
    parser.print_usage()
    return 2


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, zipfile.BadZipFile, SigningError) as error:
        print('artifact_signing: ERROR: ' + str(error))
        raise SystemExit(1)
