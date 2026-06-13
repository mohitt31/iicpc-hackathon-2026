// MALICIOUS FIXTURE — fork bomb. Expected: REJECTED at INTAKE (layer 1: the
// static scan bans `fork`). It never reaches the build or the microVM.
#include <unistd.h>
int main() { for (;;) fork(); return 0; }
