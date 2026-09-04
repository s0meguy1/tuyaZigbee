# field-artifacts

A **local staging area**, not a distribution channel. Nothing binary in this
directory is tracked, and the `.gitignore` enforces that.

## Where the firmware actually is

Prebuilt images are published as **GitHub Releases**, with a sha256 for each
file:

<https://github.com/s0meguy1/tuyaZigbee/releases>

Firmware is not committed to this repository. A rebuild of identical sources
differs in its embedded date code and trailing CRC, so a committed binary would
quietly stop matching what is actually deployed — and an image whose provenance
cannot be verified has already bricked a fixture on this project.

## What used to be here

Three build-04 era images were tracked here as forensic evidence, preserved in
commit `315f78e` because those were the exact bytes flashed to a fixture that
hung in the field on 2026-08-15.

They have been removed from the working tree. They remain in git history, and
that is deliberate — they are evidence. But they must **never** be installed:
build 04 is the build that hung the fixture, and one of the three was a
conversion image, which is exactly the file someone following the install
instructions might otherwise reach for.

Removing a file from the current tree does not erase it from history. If you
are browsing old commits, assume anything you find here is superseded and
unsafe to flash.
