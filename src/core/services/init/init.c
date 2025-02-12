#include <stdio.h>
#include <unistd.h>

int main() {
    printf("🔧 [init] Bootstrapping system...\n");

    sleep(2);
    printf("✅ [init] System initialization complete.\n");

    while (1) {
        sleep(10);
    }

    return 0;
}
