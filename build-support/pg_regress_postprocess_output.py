#!/usr/bin/env python

"""
Filters the given file in place, removing blocks like the following that are generated by ASAN/TSAN:

-----------------------------------------------------
Suppressions used:
  count      bytes template
      1        552 save_ps_display_args
-----------------------------------------------------

"""

import sys
import os
import re

SANITIZER_SEPARATOR_LINE = '-' * 53


def main():
    file_path = sys.argv[1]

    if not os.path.exists(file_path):
        # Auto-create files of this form: .../regress/expected/yb_char.out
        # This is convenient so we can still get sane results when adding new tests.
        if os.path.dirname(file_path).endswith('/regress/expected'):
            with open(file_path, 'w') as output_file:
                output_file.write(
                    '-- Automatically created by %s (new test?)\n' % os.path.basename(__file__))

    lines = []
    with open(file_path) as input_file:
        lines = input_file.readlines()

    # 1) Remove trailing whitespace. We will also do that to expected output files so that diff does
    #    not find any differences in the normal case.
    # 2) Also mask any uuids since they are randomly generated and will vary across runs.
    lines = [line.rstrip() for line in lines]

    def mask_uuid4s(unmasked_line):
        uuid_start_indices = []
        line_copy_with_uuid4s_masked = ""
        for m in re.finditer(
          r"[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-4[0-9A-Fa-f]{3}-[89ABab][0-9A-Fa-f]{3}-[0-9A-Fa-f]{12}",
          unmasked_line):
            uuid_start_indices.append(m.start())

        prev_index = 0
        for i in uuid_start_indices:
            line_copy_with_uuid4s_masked += \
                unmasked_line[prev_index:i] + "********-****-4***-****-************"
            prev_index = i + len("********-****-4***-****-************")

        line_copy_with_uuid4s_masked += unmasked_line[prev_index:]
        return line_copy_with_uuid4s_masked

    lines = [mask_uuid4s(line) for line in lines]

    result_lines = []
    i = 0
    skipping = False
    just_stopped_skipping = False
    while i < len(lines):
        if lines[i] == '-- YB_DATA_END':
            # Line indicates end of data. All further statements designed for maintenance
            # and must be removed from in result output.
            break

        just_started_skipping = False
        if (lines[i] == SANITIZER_SEPARATOR_LINE and
                i + 1 < len(lines) and
                lines[i + 1] == 'Suppressions used:'):
            skipping = True
            just_started_skipping = True

        if not skipping and not (
            # We skip one more empty line after the closing horizontal line of a suppressions block.
            just_stopped_skipping and not lines[i].strip()
        ):
            result_lines.append(lines[i])

        just_stopped_skipping = False

        if (skipping and
                lines[i] == SANITIZER_SEPARATOR_LINE and
                not just_started_skipping):
            skipping = False
            just_stopped_skipping = True
        i += 1

    # Remove empty lines from the end of the file.
    new_len = len(result_lines)
    while new_len > 0 and result_lines[new_len - 1].strip() == '':
        new_len -= 1
    if new_len < len(result_lines):
        result_lines = result_lines[:new_len]

    with open(file_path, 'w') as output_file:
        output_file.write("\n".join(result_lines) + "\n")


if __name__ == '__main__':
    main()
