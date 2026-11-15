#ifndef SUNSTONE_AIC8800_SUPPLICANT_RSN_H
#define SUNSTONE_AIC8800_SUPPLICANT_RSN_H
/* The cipher conditions of the PSK candidate test on an RSN element that wpa_parse_wpa_ie_rsn accepted: a CCMP group
 * cipher, CCMP among the pairwise ciphers, and management frame protection not required. The association request and
 * key installation support neither TKIP nor 802.11w. Include after common/wpa_common.h. The radio record's classifier
 * (a733_radio_rsn_classify) names the first of these that fails; checks/radio-rsn-summary.py checks the two agree. */
static inline int aic8800_rsn_ciphers_accepted(const struct wpa_ie_data *ie)
{
    return ie->group_cipher==WPA_CIPHER_CCMP&&(ie->pairwise_cipher&WPA_CIPHER_CCMP)&&!(ie->capabilities&WPA_CAPABILITY_MFPR);
}
#endif
