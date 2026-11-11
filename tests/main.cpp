#include "test_common.h"

void RunHandTests();
void RunWristHudTests();
void RunPoseTests();
void RunReloadTests();
void RunBodyTests();

int main()
{
	RunHandTests();
	RunWristHudTests();
	RunPoseTests();
	RunReloadTests();
	RunBodyTests();

	printf("\n%d checks, %d failed\n", g_TestChecks, g_TestFailures);
	return g_TestFailures == 0 ? 0 : 1;
}
