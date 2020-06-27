#include <kern/e1000.h>
#include <kern/pmap.h>
#include <kern/pci.h>

#include <inc/stdio.h>
#include <inc/error.h>
#include <inc/string.h>

// LAB 6: Your driver code here
volatile uint32_t *device_register_map;

#define NIC_REG(offset) (device_register_map[offset / 4])
#define TX_QUEUE_SIZE 64
#define RX_QUEUE_SIZE 128

struct eth_packet_buffer
{
	char data[DATA_PACKET_BUFFER_SIZE];
};

// transmit buffers
struct eth_packet_buffer tx_queue_data[TX_QUEUE_SIZE];
struct e1000_tx_desc *tx_queue_desc;

// receive buffers
struct eth_packet_buffer rx_queue_data[RX_QUEUE_SIZE];
struct e1000_rx_desc *rx_queue_desc;

int e1000_attach(struct pci_func *pcif) 
{
	pci_func_enable(pcif);
	device_register_map = mmio_map_region(pcif->reg_base[0], pcif->reg_size[0]);

	// Transmit initialization

	// Allocate a region of memory for the transmit descriptor list. 
	// Software should insure this memory is aligned on a paragraph (16-byte) boundary
	struct PageInfo *p = page_alloc(ALLOC_ZERO);

	// Program the Transmit Descriptor Base Address (TDBAL/TDBAH) register(s) with the address of the region. 
	// TDBAL is used for 32-bit addresses and both TDBAL and TDBAH are used for 64-bit addresses.
	// [ Note that the NIC needs physical address directly due to DMA not going through CPU. ]
	physaddr_t tx_queue_base = page2pa(p);
	NIC_REG(E1000_TDBAL) = tx_queue_base;
	NIC_REG(E1000_TDBAH) = 0;

	// Set the Transmit Descriptor Length (TDLEN) register to the size (in bytes) of the descriptor ring. 
	// This register must be 128-byte aligned.
	NIC_REG(E1000_TDLEN) = TX_QUEUE_SIZE * sizeof(struct e1000_tx_desc);

	tx_queue_desc = page2kva(p);

	for (int i = 0; i < TX_QUEUE_SIZE; i++) {
		tx_queue_desc[i].addr = (uint64_t)PADDR(&tx_queue_data[i]);
		tx_queue_desc[i].cmd = (1 << 3) | (1 << 0);
		tx_queue_desc[i].status |= E1000_TXD_STAT_DD;
	}

	// The Transmit Descriptor Head and Tail (TDH/TDT) registers are initialized 
	// (by hardware) to 0b after a power-on or a software initiated Ethernet controller reset. 
	// Software should write 0b to both these registers to ensure this.
	NIC_REG(E1000_TDH) = 0;
	NIC_REG(E1000_TDT) = 0;

	// Initialize the Transmit Control Register (TCTL) for desired operation to include the following:
	// - Set the Enable (TCTL.EN) bit to 1b for normal operation.
	// - Set the Pad Short Packets (TCTL.PSP) bit to 1b.
	NIC_REG(E1000_TCTL) |= (E1000_TCTL_EN | E1000_TCTL_PSP);

	// - Configure the Collision Threshold (TCTL.CT) to the desired value. Ethernet standard is 10h. 
	//   This setting only has meaning in half duplex mode.
	// - Configure the Collision Distance (TCTL.COLD) to its expected value. For full duplex operation, this value should be set to 40h.
	//   For gigabit half duplex, this value should be set to 200h. For 10/100 half duplex, this value should be set to 40h
	NIC_REG(E1000_TCTL) |= E1000_TCTL_COLD & ( 0x40 << 12);  // set the cold
	NIC_REG(E1000_TIPG) = 10; // page 313

	// rx initialization
	// Program the Receive Address Register(s) (RAL/RAH) with the desired Ethernet addresses.
	// 52:54:00:12:34:56
	NIC_REG(E1000_RAL) = 0x12005452;
	*(uint16_t *)&NIC_REG(E1000_RAH) = 0x5634;
	NIC_REG(E1000_RAH) |= E1000_RAH_AV;

	// Initialize the MTA (Multicast Table Array) to 0b.
	NIC_REG(E1000_MTA) = 0;

	// no interrupts as of yet
	// Allocate a region of memory for the receive descriptor list. Software should insure this memory
	// is aligned on a paragraph (16-byte) boundary. Program the Receive Descriptor Base Address (RDBAL/RDBAH) 
	// register(s) with the address of the region
	struct PageInfo *rx_desc_pg = page_alloc(ALLOC_ZERO);
	rx_queue_desc = page2kva(rx_desc_pg);
	physaddr_t rx_desc_base = page2pa(rx_desc_pg);

	NIC_REG(E1000_RDBAL) = rx_desc_base;
	NIC_REG(E1000_RDBAH) = 0;

	// Set the Receive Descriptor Length (RDLEN) register to the size (in bytes) of the descriptor ring. 
	// This register must be 128-byte aligned.
	NIC_REG(E1000_RDLEN) = RX_QUEUE_SIZE * sizeof(struct e1000_rx_desc);

	// initialize head and tail such that (tail + 1) % size = head
	NIC_REG(E1000_RDH) = 0;
	NIC_REG(E1000_RDT) = RX_QUEUE_SIZE - 1;
	for (int i = 0; i < RX_QUEUE_SIZE; i++) {
		rx_queue_desc[i].addr = (uint64_t)PADDR(&rx_queue_data[i]);
		// clear Descriptor Done so we know we are not allowed to read it
		rx_queue_desc[i].status &= ~E1000_RXD_STAT_DD;
	}
	// enable and strip CRC
	NIC_REG(E1000_RCTL) |= E1000_RCTL_EN | E1000_RCTL_SECRC;

	return 0;
}

int tx_packet(char *buf, int size) 
{
	assert(size <= ETH_MAX_PACKET_SIZE);
	int tail_indx = NIC_REG(E1000_TDT);

	if (!(tx_queue_desc[tail_indx].status & E1000_TXD_STAT_DD))
		return -E_NIC_BUSY;

	tx_queue_desc[tail_indx].status &= ~E1000_TXD_STAT_DD;
	memmove(&tx_queue_data[tail_indx].data, buf, size);
	tx_queue_desc[tail_indx].length = size;

	NIC_REG(E1000_TDT) = (tail_indx + 1) % TX_QUEUE_SIZE;
	return 0;
}

int rx_packet(char *buf, int size) 
{
	int next_indx = (NIC_REG(E1000_RDT)+1) % RX_QUEUE_SIZE;
	if (!(rx_queue_desc[next_indx].status & E1000_TXD_STAT_DD))
		return -E_RX_EMPTY;

	rx_queue_desc[next_indx].status &= ~E1000_TXD_STAT_DD;
	int rx_size = MIN(rx_queue_desc[next_indx].length, size);
	memmove(buf, rx_queue_data[next_indx].data, rx_size);
	NIC_REG(E1000_RDT) = next_indx;
	return rx_size;
}
