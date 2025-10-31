import sys
import re
import os
import subprocess

# Version file path
VERSION_FILE_PATH = '../component/base/C2RKVersion.h'

def update_commit_message_with_version(version_line, version_file_path):
    """Insert version information into the commit message"""
    result = subprocess.run(
        ['git', 'log', '-1', '--pretty=%B'],
        capture_output=True, text=True, check=True
    )

    msg_lines = result.stdout.strip().split('\n')

    # Find insertion point
    insertion_index = -1
    for i, line in enumerate(msg_lines):
        if "Change-Id:" in line:
            insertion_index = i - 1
            break

    if insertion_index != -1:
        msg_lines.insert(insertion_index, version_line)
    else:
        msg_lines.append(version_line)

    new_msg = '\n'.join(msg_lines).strip()
    subprocess.run(['git',  'add', version_file_path], check=True)
    subprocess.run(['git',  'commit', '--amend', '-m', new_msg], check=True)

def read_version_file(file_path):
    """Read the content of the version file"""
    try:
        with open(file_path, 'r') as file:
            return file.read()
    except FileNotFoundError:
        print(f"Error: File {VERSION_FILE_PATH} not found.")
        sys.exit(1)

def write_version_file(content, file_path):
    """Write content to the version file"""
    with open(file_path, 'w') as file:
        file.write(content)

def extract_versions(content):
    """Extract version numbers from file content"""
    major_match   = re.search(r'#define C2_MAJOR_VERSION\s+(\d+)', content)
    minor_match   = re.search(r'#define C2_MINOR_VERSION\s+(\d+)', content)
    revison_match = re.search(r'#define C2_REVIS_VERSION\s+(\d+)', content)
    build_match   = re.search(r'#define C2_BUILD_VERSION\s+(\d+)', content)

    if not all([major_match, minor_match, revison_match, build_match]):
        raise ValueError("Version file format error.")

    return {
        'major': int(major_match.group(1)),
        'minor': int(minor_match.group(1)),
        'revison': int(revison_match.group(1)),
        'build': int(build_match.group(1)),
    }

def increment_version(versions):
    """Increment version numbers according to rules"""
    versions['build'] += 1
    if versions['build'] > 9:
        versions['build'] = 0
        versions['revison'] += 1
        if versions['revison'] > 20:
            versions['revison'] = 0
            versions['minor'] += 1
            if versions['minor'] > 20:
                versions['minor'] = 0
                versions['major'] += 1

def replace_version_in_content(content, versions):
    """Replace version numbers in the file"""
    replacements = {
        'C2_MAJOR_VERSION': versions['major'],
        'C2_MINOR_VERSION': versions['minor'],
        'C2_REVIS_VERSION': versions['revison'],
        'C2_BUILD_VERSION': versions['build'],
    }

    for key, value in replacements.items():
        content = re.sub(
            rf'#define {key}\s+\d+',
            f'#define {key} \t{value}',
            content
        )
    return content

def update_version(versions):
    """Increment version numbers according to rules"""
    versions['build'] += 1
    if versions['build'] > 9:
        versions['build'] = 0
        versions['revison'] += 1
        if versions['revison'] > 20:
            versions['revison'] = 0
            versions['minor'] += 1
            if versions['minor'] > 20:
                versions['minor'] = 0
                versions['major'] += 1

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    file_path = os.path.join(script_dir,  VERSION_FILE_PATH)
    file_path = os.path.abspath(file_path)

    content = read_version_file(file_path)
    versions = extract_versions(content)

    increment_version(versions)

    content = replace_version_in_content(content, versions)
    write_version_file(content, file_path)

    version_str = f"Version: {versions['major']}.{versions['minor']}.{versions['revison']}_[{versions['build']}]"
    print(version_str)

    update_commit_message_with_version(version_str, file_path)

if __name__ == '__main__':
    main()
