#include <stdbool.h>  // bool
#include <string.h>   // memset

#include "os.h"
#include "glyphs.h"
#include "os_io_seproxyhal.h"
#include "nbgl_use_case.h"
#include "io.h"
#include "bip32.h"
#include "format.h"

#include "display.h"
#include "globals.h"
#include "sw.h"
#include "common_ui.h"
#include "menu.h"
#include "abi_calldata.h"
#include "ui_utils.h"

#define TITLE_MSG_LEN  50
#define FINISH_MSG_LEN 50

static nbgl_contentTagValue_t g_pairs[1];
static nbgl_contentTagValueList_t g_pairsList;

typedef enum { PARAMETER_CONFIRMATION, SELECTOR_CONFIRMATION } e_confirmation_type;

static e_confirmation_type g_current_screen;
static uint8_t g_param_index;
static uint8_t g_param_count;

char g_titleMsg[TITLE_MSG_LEN];
char g_finishMsg[FINISH_MSG_LEN];
char parameterInfo[100];

static char g_selector[9];
static char g_parameter[140];

static void buildScreen(e_confirmation_type confirm_type);

static uint32_t split_binary_parameter_part(char *result, size_t result_size, const uint8_t *parameter) {
    uint32_t i;
    for (i = 0; i < 8; i++) {
        if (parameter[i] != 0x00) {
            break;
        }
    }
    if (i == 8) {
        result[0] = '0';
        result[1] = '0';
        result[2] = '\0';
        return 2;
    } else {
        format_hex(parameter + i, 8 - i, result, result_size);
        return ((8 - i) * 2);
    }
}

static void reviewChoice(bool confirm) {
    if (!confirm) {
        G_context.state = STATE_NONE;
        io_send_sw(SW_DENY);   
        nbgl_useCaseReviewStatus(STATUS_TYPE_TRANSACTION_REJECTED, ui_menu_main);
    }

    if(g_current_screen == SELECTOR_CONFIRMATION) {
        g_current_screen = PARAMETER_CONFIRMATION;
        g_param_index = 0;
        if(g_param_count > 0) {
            buildScreen(PARAMETER_CONFIRMATION);
        } else {
            ui_display_transaction();
        }
    } else {
        g_param_index++;
        if(g_param_index < g_param_count) {
            buildScreen(PARAMETER_CONFIRMATION);
        } else {
            ui_display_transaction();
        }
    }
}

static void buildScreen(e_confirmation_type confirm_type) {
    g_pairs[0].item = (confirm_type == PARAMETER_CONFIRMATION) ? "Parameter" : "Selector";

    if(confirm_type == PARAMETER_CONFIRMATION) {
        if(g_param_count > 0) {
            const abi_param_t *param = abi_calldata_get_node(G_context.tx_info.calldata.params[g_param_index]);
            uint32_t offset = 0;
            uint32_t i;
            for (i = 0; i < 8; i++) {
                offset += split_binary_parameter_part(g_parameter + offset,
                                                        sizeof(g_parameter) - offset,
                                                        param->data + 8 * i);
                if (i != 3) {
                    g_parameter[offset++] = ':';
                }
            }
            g_parameter[strlen(g_parameter) - 1] = '\0';
        }
        g_pairs[0].value = g_parameter;
    } else {
        bytes_to_hex_string(G_context.tx_info.calldata.selector, 4, g_selector);
        for(unsigned int i = 0; i < strlen(g_selector); ++i) {
            if(g_selector[i] >= 65) {
                g_selector[i] -= 32;
            }
        }
        g_pairs[0].value = g_selector;
    }

    snprintf(g_titleMsg,
             TITLE_MSG_LEN,
             "Verify %s\n",
             (confirm_type == PARAMETER_CONFIRMATION) ? "parameter" : "selector");
    // Finish text: replace "Verify" by "Confirm"
    snprintf(g_finishMsg,
             FINISH_MSG_LEN,
             "Confirm %s\n",
             (confirm_type == PARAMETER_CONFIRMATION) ? "parameter" : "selector");

    g_pairsList.nbMaxLinesForValue = 0;
    g_pairsList.nbPairs = 1;
    g_pairsList.pairs = g_pairs;

    nbgl_useCaseReview(TYPE_TRANSACTION,
                       &g_pairsList,
                       &ICON_APP_BOILERPLATE,
                       g_titleMsg,
                       NULL,
                       g_finishMsg,
                       reviewChoice);
}



int ui_confirm_parameter(void) {
    buildScreen(PARAMETER_CONFIRMATION);
    return 0;
}

int ui_confirm_selector(void) {
    buildScreen(SELECTOR_CONFIRMATION);
    return 0;
}

void ui_contract_call_init(uint8_t param_count) {
    g_current_screen = SELECTOR_CONFIRMATION;
    g_param_index  = 0;
    g_param_count = param_count;
}