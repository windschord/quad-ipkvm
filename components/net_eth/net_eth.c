#include "net_eth.h"
#include "esp_log.h"
#include "net_eth_util.h"
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_phy.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "net_eth";

// Hardware constants (TODO(hw-verify): Read from schematic and verify against ESP32-P4 EMAC defaults)
// These values match the ETH_ESP32_EMAC_DEFAULT_CONFIG defaults for ESP32-P4.
#define ETH_MDC_GPIO       31
#define ETH_MDIO_GPIO      52
#define ETH_RMII_CLK_GPIO  50
#define ETH_TX_EN_GPIO     49
#define ETH_TXD0_GPIO      34
#define ETH_TXD1_GPIO      35
#define ETH_CRS_DV_GPIO    28
#define ETH_RXD0_GPIO      29
#define ETH_RXD1_GPIO      30

// PHY configuration
#define ETH_PHY_RESET_GPIO 51
#define ETH_PHY_ADDR       ESP_ETH_PHY_ADDR_AUTO

// volatile is used because these variables are accessed across tasks (event loop vs caller)
// exact lock-free exclusion is not strictly required here.
static volatile net_eth_state_t s_eth_state = NET_ETH_STATE_DOWN;
static volatile net_eth_cb_t s_eth_cb = NULL;

static esp_eth_handle_t s_eth_handle = NULL;
static volatile uint32_t s_ip_addr = 0;


net_eth_state_t net_eth_get_state(void)
{
    return s_eth_state;
}

void net_eth_register_callback(net_eth_cb_t cb)
{
    s_eth_cb = cb;
}

static void update_state(net_eth_state_t new_state)
{
    s_eth_state = new_state;
    if (s_eth_cb) {
        s_eth_cb(s_eth_state);
    }
}

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    uint8_t mac_addr[6] = {0};
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        ESP_LOGI(TAG, "Ethernet Link Up");
        ESP_LOGI(TAG, "Ethernet HW Addr %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);

        uint32_t phy_addr = 0;
        eth_speed_t speed = ETH_SPEED_10M;
        eth_duplex_t duplex = ETH_DUPLEX_HALF;

        if (esp_eth_ioctl(eth_handle, ETH_CMD_G_PHY_ADDR, &phy_addr) == ESP_OK) {
            ESP_LOGI(TAG, "Ethernet PHY Addr: %d", (int)phy_addr);
        }
        if (esp_eth_ioctl(eth_handle, ETH_CMD_G_SPEED, &speed) == ESP_OK) {
            ESP_LOGI(TAG, "Ethernet Speed: %s", speed == ETH_SPEED_100M ? "100Mbps" : "10Mbps");
        }
        if (esp_eth_ioctl(eth_handle, ETH_CMD_G_DUPLEX_MODE, &duplex) == ESP_OK) {
            ESP_LOGI(TAG, "Ethernet Duplex: %s", duplex == ETH_DUPLEX_FULL ? "Full" : "Half");
        }

        update_state(NET_ETH_STATE_LINK_UP);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Ethernet Link Down");
        update_state(NET_ETH_STATE_DOWN);
        s_ip_addr = 0;
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet Started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet Stopped");
        update_state(NET_ETH_STATE_DOWN);
        s_ip_addr = 0;
        break;
    default:
        break;
    }
}

static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    ESP_LOGI(TAG, "Ethernet Got IP Address");
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    ESP_LOGI(TAG, "ETHIP:" IPSTR, IP2STR(&ip_info->ip));
    ESP_LOGI(TAG, "ETHMASK:" IPSTR, IP2STR(&ip_info->netmask));
    ESP_LOGI(TAG, "ETHGW:" IPSTR, IP2STR(&ip_info->gw));
    ESP_LOGI(TAG, "~~~~~~~~~~~");

    s_ip_addr = ip_info->ip.addr;
    update_state(NET_ETH_STATE_GOT_IP);
}

esp_err_t net_eth_init(void)
{
    static bool s_is_initialized = false;
    esp_err_t ret;

    if (s_is_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    s_eth_handle = NULL;

    // Initialize network interface
    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Initialize default event loop
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&cfg);
    if (eth_netif == NULL) {
        ESP_LOGE(TAG, "esp_netif_new failed");
        return ESP_FAIL;
    }

    esp_eth_mac_t *mac = NULL;
    esp_eth_phy_t *phy = NULL;
    esp_eth_handle_t eth_handle = NULL;
    void *glue = NULL;
    bool eth_event_registered = false;
    bool ip_event_registered = false;

    // Configure EMAC (MAC)
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();

    // Explicitly set pins that correspond to ESP32-P4 default, but localized for visibility
    esp32_emac_config.smi_gpio.mdc_num = ETH_MDC_GPIO;
    esp32_emac_config.smi_gpio.mdio_num = ETH_MDIO_GPIO;
    esp32_emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    esp32_emac_config.clock_config.rmii.clock_gpio = ETH_RMII_CLK_GPIO;

    esp32_emac_config.emac_dataif_gpio.rmii.tx_en_num = ETH_TX_EN_GPIO;
    esp32_emac_config.emac_dataif_gpio.rmii.txd0_num = ETH_TXD0_GPIO;
    esp32_emac_config.emac_dataif_gpio.rmii.txd1_num = ETH_TXD1_GPIO;
    esp32_emac_config.emac_dataif_gpio.rmii.crs_dv_num = ETH_CRS_DV_GPIO;
    esp32_emac_config.emac_dataif_gpio.rmii.rxd0_num = ETH_RXD0_GPIO;
    esp32_emac_config.emac_dataif_gpio.rmii.rxd1_num = ETH_RXD1_GPIO;

    mac = esp_eth_mac_new_esp32(&esp32_emac_config, &mac_config);
    if (!mac) {
        ESP_LOGE(TAG, "esp_eth_mac_new_esp32 failed");
        ret = ESP_FAIL;
        goto err_mac;
    }

    // Configure PHY (Generic)
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = ETH_PHY_ADDR;
    phy_config.reset_gpio_num = ETH_PHY_RESET_GPIO;

    phy = esp_eth_phy_new_generic(&phy_config);
    if (!phy) {
        ESP_LOGE(TAG, "esp_eth_phy_new_generic failed");
        ret = ESP_FAIL;
        goto err_phy;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ret = esp_eth_driver_install(&eth_config, &eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_driver_install failed: %s", esp_err_to_name(ret));
        goto err_driver;
    }
    s_eth_handle = eth_handle;

    // Attach Ethernet driver to TCP/IP stack
    glue = esp_eth_new_netif_glue(eth_handle);
    if (!glue) {
        ESP_LOGE(TAG, "esp_eth_new_netif_glue failed");
        ret = ESP_FAIL;
        goto err_glue;
    }

    ret = esp_netif_attach(eth_netif, glue);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_attach failed: %s", esp_err_to_name(ret));
        goto err_attach;
    }

    // Register user defined event handers
    ret = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_event_handler_register for ETH_EVENT failed: %s", esp_err_to_name(ret));
        goto err_event;
    }
    eth_event_registered = true;

    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_event_handler_register for IP_EVENT failed: %s", esp_err_to_name(ret));
        goto err_event;
    }
    ip_event_registered = true;

    // Start Ethernet driver state machine
    ret = esp_eth_start(eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_start failed: %s", esp_err_to_name(ret));
        goto err_start;
    }

    s_is_initialized = true;
    ESP_LOGI(TAG, "Ethernet initialized successfully");
    return ESP_OK;

err_start:
err_event:
    if (ip_event_registered) {
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler);
    }
    if (eth_event_registered) {
        esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler);
    }
err_attach:
    if (glue) esp_eth_del_netif_glue(glue);
err_glue:
    if (eth_handle) esp_eth_driver_uninstall(eth_handle);
err_driver:
    if (phy) phy->del(phy);
err_phy:
    if (mac) mac->del(mac);
err_mac:
    if (eth_netif) esp_netif_destroy(eth_netif);
    s_eth_handle = NULL;
    s_is_initialized = false;
    return ret;
}

bool net_eth_get_ip_str(char *buf, size_t len)
{
    if (s_eth_state != NET_ETH_STATE_GOT_IP || s_ip_addr == 0) {
        if (buf && len > 0) {
            buf[0] = '\0';
        }
        return false;
    }
    return net_eth_ip4_to_str(s_ip_addr, buf, len);
}

int net_eth_get_speed_mbps(void)
{
    if (!s_eth_handle || (s_eth_state != NET_ETH_STATE_LINK_UP && s_eth_state != NET_ETH_STATE_GOT_IP)) {
        return 0;
    }
    eth_speed_t speed = ETH_SPEED_10M;
    if (esp_eth_ioctl(s_eth_handle, ETH_CMD_G_SPEED, &speed) == ESP_OK) {
        return speed == ETH_SPEED_100M ? 100 : 10;
    }
    return 0;
}
