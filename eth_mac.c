
#include "sdkconfig.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "eth_main.h"
#include "w5100.h"
#include "w5100_spi.h"
#include "w5100_socket.h"

static const char *TAG = "w5100_eth_mac";
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

typedef struct
{
	esp_eth_mac_t parent;
	esp_eth_mediator_t *eth;
	TaskHandle_t rx_task_hdl;
	uint8_t addr[ 6 ];
} emac_w5100_t;

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

static esp_err_t w5100_stop( emac_w5100_t *emac )
{
	w5100_close();

	return ESP_OK;
}

static esp_err_t emac_w5100_set_addr( esp_eth_mac_t *mac, uint8_t *addr )
{
	esp_err_t ret = ESP_OK;
	MAC_CHECK( addr, "can't set mac addr to null", out, ESP_ERR_INVALID_ARG );
	emac_w5100_t *emac = __containerof( mac, emac_w5100_t, parent );
	memcpy( emac->addr, addr, 6 );
	wiz_write_buf( SHAR0, addr, 6 );

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
	uint8_t status = 0;
	uint8_t *buffer;
	uint32_t length = 0;

	ulTaskNotifyTake( pdTRUE, portMAX_DELAY );

	while ( 1 )
	{
		// read interrupt status
		status = getS0_IR();

		/* packet received */
		if ( status & S0_IR_RECV )
		{
			assert( length = w5100_recv_header() );
			assert( buffer = malloc( length ) );
			if ( emac->parent.receive( &emac->parent, buffer, &length ) == ESP_OK )
			{
				/* pass the buffer to stack (e.g. TCP/IP layer) */
				if ( length )
					ESP_ERROR_CHECK( emac->eth->stack_input( emac->eth, buffer, length ) );
				else
					ESP_LOGE( TAG, "LENGTH = 0" );
			}
			else
				ESP_LOGE( TAG, "Failed to recv data from ETH" );

			// free(buffer);
		}
#if CONFIG_EMAC_RECV_TASK_ENABLE_DELAY
		vTaskDelay( CONFIG_EMAC_DELAY_TICKS );
#endif
	}
}

static esp_err_t emac_w5100_set_link( esp_eth_mac_t *mac, eth_link_t link )
{
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
	uint8_t mode_register = IINCHIP_READ( S0_MR );

	if ( enable && !( mode_register & S0_MR_MF ) )
		IINCHIP_WRITE( S0_MR, mode_register | S0_MR_MF );
	else if ( !enable && ( mode_register & S0_MR_MF ) )
		IINCHIP_WRITE( S0_MR, mode_register & ~S0_MR_MF );

	return ESP_OK;
}

static esp_err_t emac_w5100_transmit( esp_eth_mac_t *mac, uint8_t *buf, uint32_t length )
{
	while ( length > SSIZE )
	{
		w5100_send( buf, SSIZE );
		length -= SSIZE;
	}

	w5100_send( buf, ( uint16_t )length );

	return ESP_OK;
}

static esp_err_t emac_w5100_receive( esp_eth_mac_t *mac, uint8_t *buf, uint32_t *length )
{
	*length = w5100_recv( buf );

	return *length ? ESP_OK : ESP_FAIL;
}

static esp_err_t emac_w5100_init( esp_eth_mac_t *mac )
{
	esp_err_t ret = ESP_OK;
	emac_w5100_t *emac = __containerof( mac, emac_w5100_t, parent );
	esp_eth_mediator_t *eth = emac->eth;

	ESP_ERROR_CHECK( esp_read_mac( emac->addr, ESP_MAC_ETH ) );
	iinchip_init( emac->addr );

	MAC_CHECK( eth->on_state_changed( eth, ETH_STATE_LLINIT, NULL ) == ESP_OK, "lowlevel init failed", out, ESP_FAIL );

	xTaskNotifyGive( emac->rx_task_hdl );

	return ret;
out:
	eth->on_state_changed( eth, ETH_STATE_DEINIT, NULL );

	return ret;
}

static esp_err_t emac_w5100_deinit( esp_eth_mac_t *mac )
{
	emac_w5100_t *emac = __containerof( mac, emac_w5100_t, parent );
	esp_eth_mediator_t *eth = emac->eth;

	w5100_stop( emac );

	// TODO

	eth->on_state_changed( eth, ETH_STATE_DEINIT, NULL );

	return ESP_OK;
}

static esp_err_t emac_w5100_del( esp_eth_mac_t *mac )
{
	emac_w5100_t *emac = __containerof( mac, emac_w5100_t, parent );
	vTaskDelete( emac->rx_task_hdl );
	free( emac );

	return ESP_OK;
}

esp_eth_mac_t *esp_eth_mac_new_w5100( const eth_mac_config_t *mac_config )
{
	esp_eth_mac_t *ret = NULL;
	emac_w5100_t *emac = NULL;
	MAC_CHECK( mac_config, "can't set mac config to null", err, NULL );
	emac = calloc( 1, sizeof( emac_w5100_t ) );
	MAC_CHECK( emac, "calloc emac failed", err, NULL );

	emac->parent.set_mediator = emac_w5100_set_mediator;
	emac->parent.init = emac_w5100_init;
	emac->parent.deinit = emac_w5100_deinit;
	emac->parent.del = emac_w5100_del;
	emac->parent.write_phy_reg = emac_w5100_write_phy_reg;
	emac->parent.read_phy_reg = emac_w5100_read_phy_reg;
	emac->parent.set_addr = emac_w5100_set_addr;
	emac->parent.get_addr = emac_w5100_get_addr;
	emac->parent.set_speed = emac_w5100_set_speed;
	emac->parent.set_duplex = emac_w5100_set_duplex;
	emac->parent.set_link = emac_w5100_set_link;
	emac->parent.set_promiscuous = emac_w5100_set_promiscuous;
	emac->parent.transmit = emac_w5100_transmit;
	emac->parent.receive = emac_w5100_receive;

	BaseType_t xReturned = xTaskCreatePinnedToCore( emac_w5100_task,
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