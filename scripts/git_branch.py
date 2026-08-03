"""
PlatformIO pre-build script: derive CROSSPOINT_VERSION from git tags.

Git tags are the single source of truth for the version — platformio.ini does
not carry one. Releases are cut by tagging (see .github/workflows/
release-dispatch.yml), so there is no version to keep in sync by hand.

    release env, on a tag   1.2.3
    release env, off-tag    1.2.3-4-gabc1234      (4 commits past v1.2.3)
    dev env                 1.2.3-dev-my-branch-abc1234

An environment that sets CROSSPOINT_VERSION in build_flags keeps its own value;
platformio.local.ini uses that to pin a version for OTA testing.
"""

import os
import subprocess
import sys


def warn(msg):
    print(f'WARNING [git_branch.py]: {msg}', file=sys.stderr)


def run_git_value(project_dir, args, label):
    try:
        value = subprocess.check_output(
            ['git', *args],
            text=True, stderr=subprocess.PIPE, cwd=project_dir
        ).strip()
        # Strip characters that would break a C string literal
        return ''.join(c for c in value if c not in '"\\')
    except FileNotFoundError:
        warn(f'git not found on PATH; {label} suffix will be "unknown"')
        return 'unknown'
    except subprocess.CalledProcessError as e:
        warn(
            f'git command failed (exit {e.returncode}): '
            f'{e.stderr.strip()}; {label} suffix will be "unknown"'
        )
        return 'unknown'
    except OSError as e:
        warn(
            f'OS error reading git {label}: {e}; '
            f'{label} suffix will be "unknown"'
        )
        return 'unknown'
    except Exception as e:  # pylint: disable=broad-exception-caught
        warn(
            f'Unexpected error reading git {label}: {e}; '
            f'{label} suffix will be "unknown"'
        )
        return 'unknown'


def get_git_branch(project_dir):
    branch = run_git_value(
        project_dir, ['rev-parse', '--abbrev-ref', 'HEAD'], 'branch'
    )
    # Detached HEAD has no branch name.
    if branch == 'HEAD':
        return 'detached'
    return branch


def get_git_short_sha(project_dir):
    return run_git_value(
        project_dir, ['rev-parse', '--short', 'HEAD'], 'short SHA'
    )


def get_version_from_git_tag(project_dir, exact=False, bare=False):
    """Derive the version from git tags. Git tags are the single source of truth.

    exact=True returns a version only when HEAD sits exactly on a release tag
    (v1.2.3 -> 1.2.3), and None otherwise.
    bare=True returns just the nearest tag (1.2.3); otherwise git describe
    appends distance and commit (1.2.3-4-gabc1234), marking the build as "4
    commits past v1.2.3" rather than letting it pass for the release itself.

    Only v<digit>* is matched, so tags inherited from other forks cannot be
    picked up (they are not ancestors of this branch either).
    """
    args = ['describe', '--tags', '--match', 'v[0-9]*']
    if exact:
        args.append('--exact-match')
    elif bare:
        args.append('--abbrev=0')
    try:
        tag = subprocess.check_output(
            ['git', *args], text=True, stderr=subprocess.PIPE, cwd=project_dir
        ).strip()
        version = tag.lstrip('v')
        if version:
            return version
    except (FileNotFoundError, subprocess.CalledProcessError):
        pass
    return None


def get_base_version(project_dir, bare=False):
    # Env var override (OTA test / debug): highest priority
    override = os.environ.get('OTA_TEST_VERSION')
    if override:
        return override

    git_version = get_version_from_git_tag(project_dir, bare=bare)
    if git_version:
        return git_version

    # No tags reachable (shallow clone, fresh fork). platformio.ini no longer
    # carries a version, so there is nothing left to fall back to.
    warn('No v* git tag reachable from HEAD; base version will be "0.0.0"')
    return '0.0.0'


def inject_version(env):
    # Every environment gets its version from here. An env that already defines
    # CROSSPOINT_VERSION in build_flags (e.g. an OTA test build pinning
    # "0.1.12-rc1") keeps that value.
    for define in env.get('CPPDEFINES', []):
        name = define[0] if isinstance(define, (list, tuple)) else define
        if name == 'CROSSPOINT_VERSION':
            return

    project_dir = env['PROJECT_DIR']

    if env['PIOENV'] == 'default':
        # Dev build: base version plus branch and commit, so a firmware built
        # from a feature branch is never mistaken for a release. The branch and
        # SHA already say where the build came from, so use the bare tag name
        # rather than git describe's "-<distance>-g<sha>" suffix.
        base_version = get_base_version(project_dir, bare=True)
        branch = get_git_branch(project_dir)
        short_sha = get_git_short_sha(project_dir)
        version_string = f'{base_version}-dev-{branch}-{short_sha}'
    else:
        # Release build. On a release tag this is exactly "1.2.3"; off-tag,
        # git describe appends the distance and commit ("1.2.3-4-gabc1234") so
        # an ad-hoc release build is distinguishable from the real release.
        version_string = (
            get_version_from_git_tag(project_dir, exact=True)
            or get_base_version(project_dir)
        )

    env.Append(CPPDEFINES=[('CROSSPOINT_VERSION', f'\\"{version_string}\\"')])
    print(f'CrossPoint build version: {version_string}')


# PlatformIO/SCons entry point — Import and env are SCons builtins injected at runtime.
# When run directly with Python (e.g. for validation), a lightweight fake env is used
# so the git/version logic can be exercised without a full build.
try:
    Import('env')           # noqa: F821  # type: ignore[name-defined]
    inject_version(env)     # noqa: F821  # type: ignore[name-defined]
except NameError:
    class _Env(dict):
        def Append(self, **_): pass

    _project_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    inject_version(_Env({'PIOENV': 'default', 'PROJECT_DIR': _project_dir}))
