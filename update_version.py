import sys
import re

# get project version file
path = 'component/base/C2RKVersion.h'

with open(path, 'r') as file:
    ctx = file.read()

major_match   = re.search(r'#define C2_MAJOR_VERSION \s*(\d+)', ctx)
minor_match   = re.search(r'#define C2_MINOR_VERSION \s*(\d+)', ctx)
revison_match = re.search(r'#define C2_REVIS_VERSION \s*(\d+)', ctx)
build_match   = re.search(r'#define C2_BUILD_VERSION \s*(\d+)', ctx)

if major_match and minor_match and revison_match and build_match:
    major   = int(major_match.group(1))
    minor   = int(minor_match.group(1))
    revison = int(revison_match.group(1))
    build   = int(build_match.group(1))

    build += 1
    if build > 9:
        build = 0
        revison += 1
        if revison > 20:
            revison = 0
            minor += 1
            if minor > 20:
                minor = 0
                major += 1

    # replace version number
    ctx = re.sub(r'#define C2_MAJOR_VERSION \s*(\d+)',
                 f'#define C2_MAJOR_VERSION \t{major}', ctx)
    ctx = re.sub(r'#define C2_MINOR_VERSION \s*(\d+)',
                 f'#define C2_MINOR_VERSION \t{minor}', ctx)
    ctx = re.sub(r'#define C2_REVIS_VERSION \s*(\d+)',
                 f'#define C2_REVIS_VERSION \t{revison}', ctx)
    ctx = re.sub(r'#define C2_BUILD_VERSION \s*(\d+)',
                 f'#define C2_BUILD_VERSION \t{build}', ctx)

    with open(path, 'w') as file:
        file.write(ctx)

    print("Version: %d.%d.%d_[%d]" % (major, minor, revison, build))
else:
    print("project version file format error")
