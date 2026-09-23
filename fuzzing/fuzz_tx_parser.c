#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#include "rlp_decode.h"
#include "types.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    zond_tx_t tx;
    memset(&tx, 0, sizeof(tx));
    decode_ledger_tx(data, size, &tx);
    return 0;
}
