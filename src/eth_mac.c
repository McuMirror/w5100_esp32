
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "eth_main.h"
#include "eth_if.h"

#include "w5100.h"
#include "w5100_ll.h"
#include "w5100_socket.h"

#define MAC_CHECK( a, str, goto_tag, ret_value, ... )                               \
	do                                                                              \
	{                                                                               \
		if ( !( a ) )                                                               \
		{                                                                           \
			ESP_LOGE( TAG, "%s(%d): " str, __FUNCTION__, __LINE__, ##__VA_ARGS__ ); \
			ret = ret_value;                                                        \
			goto goto_tag;                                                          \
		}                                                                           \
	} while ( 0 )

#define W5100_TSK_RUN	  ( ( uint32_t )0 )
#define W5100_TSK_HOLD_ON ( ( uint32_t )1 )
#define W5100_TSK_GO_ON	  ( ( uint32_t )2 )
#define W5100_TSK_DELETE  ( ( uint32_t )3 )

typedef struct
{
	esp_eth_mac_t parent;
	esp_eth_mediator_t *eth;
	TaskHandle_t rx_task_hdl;
	uint8_t addr[ 6 ];
	bool promiscousModeDisabled;
} emac_w5100_t;

static const char *TAG = "w5100_eth_mac";

static esp_err_t emac_w5100_write_phy_reg( esp_eth_mac_t *mac, uint32_t phy_addr, uint32_t phy_reg, uint32_t reg_value )
{
	return ESP_OK;
}

static esp_err_t emac_w5100_read_phy_reg( esp_eth_mac_t *mac, uint32_t phy_addr, uint32_t phy_reg, uint32_t *reg_value )
{
	return ESP_OK;
}

static esp_err_t emac_w5100_set_mediator( esp_eth_mac_t *mac, esp_eth_mediator_t *eth )
{
	esp_err_t ret = ESP_OK;
	MAC_CHECK( eth, "can't set mac's mediator to null", out, ESP_ERR_INVALID_ARG );
	emac_w5100_t *emac = __containerof( mac, emac_w5100_t, parent );
	emac->eth = eth;
out:
	return ret;
}

static esp_err_t emac_w5100_start( esp_eth_mac_t *mac )
{
	emac_w5100_t *emac = __containerof( mac, emac_w5100_t, parent );
	esp_err_t ret =
		pdTRUE == xTaskNotify( emac->rx_task_hdl, W5100_TSK_GO_ON, eSetValueWithoutOverwrite ) ? ESP_OK : ESP_FAIL;
	return ret;
}

static esp_err_t emac_w5100_stop( esp_eth_mac_t *mac )
{
	return ESP_OK;
}

static esp_err_t emac_w5100_set_addr( esp_eth_mac_t *mac, uint8_t *addr )
{
	esp_err_t ret = ESP_OK;
	MAC_CHECK( addr, "can't set mac addr to null", out, ESP_ERR_INVALID_ARG );
	emac_w5100_t *emac = __containerof( mac, emac_w5100_t, parent );
	w5100_setMAC( addr );
	memcpy( emac->addr, addr, 6 );
out:
	return ret;
}

static esp_err_t emac_w5100_get_addr( esp_eth_mac_t *mac, uint8_t *addr )
{
	esp_err_t ret = ESP_OK;
	MAC_CHECK( addr, "can't set mac addr to null", out, ESP_ERR_INVALID_ARG );
	emac_w5100_t *emac = __containerof( mac, emac_w5100_t, parent );
	memcpy( addr, emac->addr, 6 );
out:
	return ret;
}

static void emac_w5100_task( void *arg )
{
	emac_w5100_t *emac = ( emac_w5100_t * )arg;
	uint8_t *buffer = NULL;
	uint32_t length = 0, notification_value = 0;
	TickType_t notif_wait_time = portMAX_DELAY;

	// watch for task termination notification
	while ( 1 )
	{
		notification_value = ulTaskNotifyTake( pdTRUE, notif_wait_time );

		if ( notification_value == W5100_TSK_RUN )
		{
			// read interrupt status and check if data arrived
			if ( w5100_wasDataReceiceved() )
			{
				ESP_ERROR_CHECK( emac->parent.receive( &emac->parent, ( uint8_t * )&buffer, &length ) );
				if ( length )
				{
					/* pass the buffer to stack (e.g. TCP/IP layer) */
					if ( ESP_OK != emac->eth->stack_input( emac->eth, buffer, length ) )
					{
						ESP_LOGE( TAG, "Failed to deliver eth data to layer3" );
						free( buffer );
					}
				}
				else
					ESP_LOGE( TAG, "LENGTH = 0" );
			}
		}
		else if ( notification_value == W5100_TSK_HOLD_ON )
		{
			ESP_LOGI( TAG, "W5100_TSK_HOLD_ON" );
			notif_wait_time = portMAX_DELAY;
		}
		else if ( notification_value == W5100_TSK_GO_ON )
		{
			ESP_LOGI( TAG, "W5100_TSK_GO_ON" );
			notif_wait_time = CONFIG_EMAC_RX_TASK_YIELD_TICKS;
		}
		else if ( notification_value == W5100_TSK_DELETE )
		{
			ESP_LOGI( TAG, "W5100_TSK_DELETE" );
			break;
		}
	}

	ESP_LOGI( TAG, "Deleting emac_w5100_task..." );
	vTaskDelete( NULL );
}

static esp_err_t emac_w5100_set_link( esp_eth_mac_t *mac, eth_link_t link )
{
	if ( link == ETH_LINK_UP )
		ESP_ERROR_CHECK( mac->start( mac ) );
	return ESP_OK;
}

static esp_err_t emac_w5100_set_speed( esp_eth_mac_t *mac, eth_speed_t speed )
{
	return ESP_OK;
}

static esp_err_t emac_w5100_set_duplex( esp_eth_mac_t *mac, eth_duplex_t duplex )
{
	return ESP_OK;
}

static esp_err_t emac_w5100_set_promiscuous( esp_eth_mac_t *mac, bool enable )
{
	emac_w5100_t *emac = __containerof( mac, emac_w5100_t, parent );

	if ( enable && emac->promiscousModeDisabled )
	{
		w5100_enablePromiscuousMode();
		emac->promiscousModeDisabled = false;
	}
	else if ( !( enable || emac->promiscousModeDisabled ) )
	{
		w5100_disablePromiscuousMode();
		emac->promiscousModeDisabled = true;
	}

	return ESP_OK;
}

static esp_err_t emac_w5100_transmit( esp_eth_mac_t *mac, uint8_t *buf, uint32_t length )
{
#if CONFIG_W5100_DEBUG_TX
	ESP_LOGD( __func__, "length = %" PRIu32, length );
	ESP_LOG_BUFFER_HEXDUMP( __func__, buf, length, ESP_LOG_DEBUG );
#endif
	w5100_socket_send( buf, ( uint16_t )length );

	return ESP_OK;
}

static esp_err_t emac_w5100_receive( esp_eth_mac_t *mac, uint8_t *buf, uint32_t *length )
{
	uint8_t **const pBuf = ( uint8_t * *const )buf;
	*length = w5100_socket_recv( pBuf );
#if CONFIG_W5100_DEBUG_RX
	ESP_LOGD( __func__, "length = %" PRIu32, *length );
	ESP_LOG_BUFFER_HEXDUMP( __func__, *pBuf, *length, ESP_LOG_DEBUG );
#endif
	return *length ? ESP_OK : ESP_FAIL;
}

static esp_err_t emac_w5100_init( esp_eth_mac_t *mac )
{
	esp_err_t ret = ESP_OK;
	emac_w5100_t *emac = __containerof( mac, emac_w5100_t, parent );
	esp_eth_mediator_t *eth = emac->eth;

	w5100_init();
	w5100_socket_open();
	ESP_ERROR_CHECK( mac->set_promiscuous( mac, false ) );

	MAC_CHECK( eth->on_state_changed( eth, ETH_STATE_LLINIT, NULL ) == ESP_OK, "lowlevel init failed", out, ESP_FAIL );

	return ret;
out:
	eth->on_state_changed( eth, ETH_STATE_DEINIT, NULL );

	return ret;
}

static esp_err_t emac_w5100_deinit( esp_eth_mac_t *mac )
{
	emac_w5100_t *emac = __containerof( mac, emac_w5100_t, parent );
	emac->eth->on_state_changed( emac->eth, ETH_STATE_DEINIT, NULL );
	esp_err_t ret =
		pdPASS == xTaskNotify( emac->rx_task_hdl, W5100_TSK_DELETE, eSetValueWithoutOverwrite ) ? ESP_OK : ESP_FAIL;
	w5100_socket_close();

	return ret;
}

static esp_err_t emac_w5100_del( esp_eth_mac_t *mac )
{
	emac_w5100_t *emac = __containerof( mac, emac_w5100_t, parent );
	free( emac );

	return ESP_OK;
}

static esp_err_t emac_w5100_enable_flow_ctrl( esp_eth_mac_t *mac, bool enable )
{
	return ESP_OK;
}

static esp_err_t emac_w5100_set_peer_pause_ability( esp_eth_mac_t *mac, uint32_t ability )
{
	return ESP_OK;
}

esp_eth_mac_t *esp_eth_mac_new_w5100( const eth_mac_config_t *const mac_config )
{
	esp_eth_mac_t *ret = NULL;
	emac_w5100_t *emac = NULL;
	MAC_CHECK( mac_config, "can't set mac config to null", err, NULL );
	emac = calloc( 1, sizeof( emac_w5100_t ) );
	MAC_CHECK( emac, "calloc emac failed", err, NULL );

	emac->parent.set_mediator = emac_w5100_set_mediator;
	emac->parent.init = emac_w5100_init;
	emac->parent.deinit = emac_w5100_deinit;
	emac->parent.start = emac_w5100_start;
	emac->parent.stop = emac_w5100_stop;
	emac->parent.transmit = emac_w5100_transmit;
	emac->parent.receive = emac_w5100_receive;
	emac->parent.read_phy_reg = emac_w5100_read_phy_reg;
	emac->parent.write_phy_reg = emac_w5100_write_phy_reg;
	emac->parent.set_addr = emac_w5100_set_addr;
	emac->parent.get_addr = emac_w5100_get_addr;
	emac->parent.set_speed = emac_w5100_set_speed;
	emac->parent.set_duplex = emac_w5100_set_duplex;
	emac->parent.set_link = emac_w5100_set_link;
	emac->parent.set_promiscuous = emac_w5100_set_promiscuous;
	emac->parent.enable_flow_ctrl = emac_w5100_enable_flow_ctrl;
	emac->parent.set_peer_pause_ability = emac_w5100_set_peer_pause_ability;
	emac->parent.del = emac_w5100_del;

	BaseType_t xReturned = xTaskCreatePinnedToCore(
		emac_w5100_task,
		"w5100_tsk",
		mac_config->rx_task_stack_size,
		emac,
		mac_config->rx_task_prio,
		&emac->rx_task_hdl,
#if CONFIG_EMAC_RECV_TASK_ENABLE_CORE_AFFINITY
		CONFIG_EMAC_RECV_TASK_CORE
#else
		tskNO_AFFINITY
#endif
	);
	MAC_CHECK( xReturned == pdPASS, "create w5100 task failed", err, NULL );

	return &( emac->parent );
err:
	if ( emac )
	{
		if ( emac->rx_task_hdl )
			vTaskDelete( emac->rx_task_hdl );

		free( emac );
	}

	return ret;
}
