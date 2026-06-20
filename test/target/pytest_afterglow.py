"""pytest-embedded driver for the on-target Unity suite.

Boots the built test app (real hardware or QEMU) and asserts the Unity summary
reports zero failures. Run:

    idf.py -C test/target set-target esp32s3 build
    pytest test/target --embedded-services idf,qemu          # QEMU
    pytest test/target --embedded-services idf --port <dev>  # real board

Requires: pytest, pytest-embedded, pytest-embedded-idf, pytest-embedded-qemu.
"""


def test_afterglow_target_suite(dut):
    # Unity prints "Tests N Failures M Ignored K" at the end of the run.
    dut.expect_unity_test_output(timeout=120)
    # expect_unity_test_output raises on any failed case; an explicit belt-and
    # -suspenders check on the summary line:
    dut.expect(r"Tests (\d+) Failures 0 Ignored", timeout=10)
