// The I/O APIC manages hardware interrupts for an SMP system.
// http://www.intel.com/design/chipsets/datashts/29056601.pdf
// See also picirq.c.

#include "types.h"
#include "traps.h"
#include "kernel.hh"
#include "apic.hh"
#include "cpu.hh"
#include "pci.hh"
#include "kstream.hh"

static console_stream verbose(true);

#define REG_ID     0x00  // Register index: ID
#define REG_VER    0x01  // Register index: version
#define REG_TABLE  0x10  // Redirection table base

// The redirection table starts at REG_TABLE and uses
// two registers to configure each interrupt.  
// The first (low) register in a pair contains configuration bits.
// The second (high) register contains a bitmask telling which
// CPUs can serve that interrupt.
#define INT_DISABLED   0x00010000  // Interrupt disabled
#define INT_LEVEL      0x00008000  // Level-triggered (vs edge-)
#define INT_ACTIVELOW  0x00002000  // Active low (vs high)
#define INT_LOGICAL    0x00000800  // Destination is CPU id (vs APIC ID)

#define MAX_IOAPICS    8

// IO APIC MMIO structure: write reg, then read or write data.
struct ioapic_mmio {
  u32 reg;
  u32 pad[3];
  u32 data;

  u32 read(int reg) volatile;
  void write(int reg, u32 data) volatile;
};

class ioapic_82093 : public ioapic
{
  // The n'th IOAPIC services IRQs [ioapic_base[n], ioapic_lim[n]).
  volatile ioapic_mmio *ioapics[MAX_IOAPICS];
  int ioapic_base[MAX_IOAPICS];
  int ioapic_lim[MAX_IOAPICS];
  int nioapics;

  // Map from ISA IRQs to IOAPIC pins.  Identity mapped by default,
  // but can be overridden by the BIOS.
  irq isa_irqs[16];

  volatile ioapic_mmio *map_gsi(int gsi, int *pin_out);

public:
  ioapic_82093();

  void register_base(int irq_base, paddr address);
  void register_isa_irq_override(int isa_irq, irq override);
  void register_nmi(irq nmi);

  irq map_isa_irq(int isa_irq);
  irq map_pci_irq(struct pci_func *f);

protected:
  void enable_irq(const struct irq &, bool enable);
  void eoi_irq(const struct irq &);
};

u32
ioapic_mmio::read(int reg) volatile
{
  this->reg = reg;
  return this->data;
}

void
ioapic_mmio::write(int reg, u32 data) volatile
{
  this->reg = reg;
  this->data = data;
}

volatile ioapic_mmio *
ioapic_82093::map_gsi(int gsi, int *pin_out)
{
  for (int i = 0; i < nioapics; ++i)
    if (ioapic_base[i] <= gsi && gsi < ioapic_lim[i]) {
      *pin_out = gsi - ioapic_base[i];
      return ioapics[i];
    }
  return nullptr;
}

ioapic_82093::ioapic_82093()
{
  // Initialize ISA IRQ overrides to the default identity mapping.
  for (int i = 0; i < 16; ++i) {
    isa_irqs[i] = irq::default_isa();
    isa_irqs[i].vector = T_IRQ0 + i;
    isa_irqs[i].gsi = i;
  }
}

void
ioapic_82093::register_base(int irq_base, paddr address)
{
  if (nioapics == MAX_IOAPICS)
    panic("ioapic: Too many IOAPICs.  Increase MAX_IOAPICS");

  // [IOAPIC 3.2.2]
  auto ioapic = (volatile struct ioapic_mmio*)p2v(address);
  ioapics[nioapics] = ioapic;
  int maxintr = (ioapic->read(REG_VER) >> 16) & 0xFF;

  ioapic_base[nioapics] = irq_base;
  ioapic_lim[nioapics] = irq_base + maxintr + 1;

  verbose.println("ioapic: IOAPIC for IRQs ", irq_base, "..",
                  irq_base + maxintr, " at ", shex(address));

  // Mark all interrupts edge-triggered, active high, disabled,
  // and not routed to any CPUs.
  for(int i = 0; i <= maxintr; i++) {
    ioapic->write(REG_TABLE+2*i, INT_DISABLED | (T_IRQ0 + i));
    ioapic->write(REG_TABLE+2*i+1, 0);
  }

  ++nioapics;
}

void
ioapic_82093::register_isa_irq_override(int isa_irq, irq override)
{
  if (isa_irq >= 16) {
    swarn.println("ioapic: Cannot override non-legacy IRQ ", isa_irq);
    return;
  }
  override.vector = T_IRQ0 + override.gsi;
  isa_irqs[isa_irq] = override;
}

void
ioapic_82093::register_nmi(irq nmi)
{
  swarn.println("ioapic: register_nmi not implemented");
}

irq
ioapic_82093::map_isa_irq(int isa_irq)
{
  assert(isa_irq < 16);
  return isa_irqs[isa_irq];
}

irq
ioapic_82093::map_pci_irq(struct pci_func *f)
{
  // XXX(austin) Totally bogus, happens to work on QEMU and josmp
  swarn.println("ioapic: Assuming IOAPIC routing of PCI IRQs matches legacy PIC");
  irq res{};
  res.vector = T_IRQ0 + f->irq_line;
  res.gsi = f->irq_line;
  return res;
}

void
ioapic_82093::enable_irq(const struct irq &irq, bool enable)
{
  assert(irq.valid());
  assert(irq.vector >= 32 && irq.vector < 256);
  assert(irq.vector != T_TLBFLUSH && irq.vector != T_SAMPCONF);

  int pin;
  auto ioapic = map_gsi(irq.gsi, &pin);
  if (!ioapic)
    panic("ioapic: Cannot enable IRQ %d, no IOAPIC for that IRQ", irq.gsi);

  // Route interrupts to CPU 0
  uint64_t dest = cpus[0].hwid.num;

  if (enable)
    verbose.println("ioapic: Routing IRQ ", irq.gsi, " to APICID ", dest);
  else
    verbose.println("ioapic: Masking IRQ ", irq.gsi);

  // [IOAPIC 3.2.4] Fixed delivery mode, physical destination mode,
  // routed to APIC ID dest.
  uint64_t reg = irq.vector | (dest << 56);
  if (irq.active_low)
    reg |= INT_ACTIVELOW;
  if (irq.level_triggered)
    reg |= INT_LEVEL;
  if (!enable)
    reg |= INT_DISABLED;
  ioapic->write(REG_TABLE+2*pin, reg & 0xFFFFFFFF);
  ioapic->write(REG_TABLE+2*pin+1, (reg >> 32) & 0xFFFFFFFF);
}

void
ioapic_82093::eoi_irq(const struct irq &irq)
{
  assert(irq.valid());
  // Assume the LAPIC is in broadcast EOI mode
  lapic->eoi();
}

bool
initextpic_ioapic(void)
{
  static ioapic_82093 ioapic;
  if (!acpi_setup_ioapic(&ioapic))
    return false;

  extpic = &ioapic;
  return true;
}
