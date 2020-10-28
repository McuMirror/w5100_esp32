
#include <assert.h>
#include <stdlib.h>

#include "w5100.h"
#include "w5100_socket.h"

void w5100_socket( bool enable_mac_filter )
{
	do
	{
		w5100_close();
		IINCHIP_WRITE( S0_MR, enable_mac_filter ? S0_MR_MACRAW | S0_MR_MF : S0_MR_MACRAW );
		IINCHIP_WRITE( S0_CR, S0_CR_OPEN );

		while ( IINCHIP_READ( S0_CR ) )
			;
	} while ( IINCHIP_READ( S0_SR ) != SOCK_MACRAW );
}

void w5100_close( void )
{
	IINCHIP_WRITE( S0_CR, S0_CR_CLOSE );

	while ( IINCHIP_READ( S0_CR ) )
		;

	IINCHIP_WRITE( S0_IR, 0xFF );
}

uint16_t w5100_send( uint8_t *buf, uint16_t len )
{
	while ( len > getS0_TX_FSR() )
		;

	send_data_processing( buf, len );

	IINCHIP_WRITE( S0_CR, S0_CR_SEND );

	while ( IINCHIP_READ( S0_CR ) )
		;

	uint8_t ir_reg;
	while ( !( ( ir_reg = IINCHIP_READ( S0_IR ) ) & S0_IR_SEND_OK ) )
		if ( ir_reg & S0_IR_TIMEOUT )
			return 0;

	return len;
}

uint16_t w5100_recv( uint8_t **buf )
{
	uint16_t data_len, ptr = read_uint16_reg( S0_RX_RD0 );

	read_data( ptr, ( uint8_t * )&data_len, 2 );
	ptr += 2;
	data_len = __builtin_bswap16( data_len ) - 2;

	*buf = malloc(data_len);
	assert(*buf);

	read_data( ptr, *buf, data_len );
	write_uint16_reg( S0_RX_RD0, ptr + data_len );

	IINCHIP_WRITE( S0_CR, S0_CR_RECV );

	while ( IINCHIP_READ( S0_CR ) )
		;

	return data_len;
}
