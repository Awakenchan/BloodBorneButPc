# SPDX-License-Identifier: GPL-2.0-or-later
"""Pass the shared online configuration to the Linux shell without evaluating shell code."""
import argparse
import contextlib
import os
import sys
from pathlib import Path

from online import ONLINE_ENV, configure_online, read_settings


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', required=True, type=Path)
    parser.add_argument('--data', required=True, type=Path)
    args = parser.parse_args()
    before = dict(os.environ)
    after = dict(before)
    try:
        settings = read_settings(args.config) if args.config.is_file() else {}
        # Informational messages belong in the log, outside the environment-change protocol.
        with contextlib.redirect_stdout(sys.stderr):
            override = configure_online(settings, args.data, after)
        changes = []
        for key in ONLINE_ENV:
            if before.get(key) == after.get(key):
                continue
            if key in after:
                value = after[key]
                if any(ord(c) < 32 for c in value):
                    raise ValueError(f'Invalid control character in {key}')
                changes.append(f'env:{key}={value}')
            else:
                changes.append(f'unset:{key}')
        # The shared configuration reuses inherited credentials; it never emits them here.
        changes.append('log:Online: configured server profile and automatic game redirect'
                       if override else 'log:Online: offline mode')
    except (OSError, ValueError) as error:
        print(f'Online configuration: {error}', file=sys.stderr)
        return 1
    print('\n'.join(changes))
    return 0


if __name__ == '__main__':
    sys.exit(main())
