#ifndef TOX_SELFTEST_H
#define TOX_SELFTEST_H
// Milestone 33: ToxUI's own self-test suite -- see tox_selftest.c's
// header comment. Returns 1 if every case passed, matching every
// kernel subsystem's own "<subsystem>_selftest()" convention.
int tox_selftest(void);
#endif // TOX_SELFTEST_H
