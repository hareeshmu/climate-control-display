#include "spi.h"
#include "esphome/core/log.h"
#include "esphome/core/gpio.h"
#include "esphome/core/application.h"

namespace esphome {
namespace spi {

const char *const TAG = "spi";

SPIDelegate *const SPIDelegate::NULL_DELEGATE =  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
    new SPIDelegateDummy();
// https://bugs.llvm.org/show_bug.cgi?id=48040

bool SPIDelegate::is_ready() { return true; }

SPIDelegate *SPIComponent::register_device(SPIClient *device, SPIMode mode, SPIBitOrder bit_order, uint32_t data_rate,
                                           GPIOPin *cs_pin) {
  if (this->devices_.count(device) != 0) {
    ESP_LOGE(TAG, "SPI device already registered");
    return this->devices_[device];
  }
  SPIDelegate *delegate = this->spi_bus_->get_delegate(data_rate, bit_order, mode, cs_pin);  // NOLINT
  this->devices_[device] = delegate;
  return delegate;
}

void SPIComponent::unregister_device(SPIClient *device) {
  if (this->devices_.count(device) == 0) {
    esph_log_e(TAG, "SPI device not registered");
    return;
  }
  delete this->devices_[device];  // NOLINT
  this->devices_.erase(device);
}

void SPIComponent::setup() {
  ESP_LOGD(TAG, "Setting up SPI bus...");

  if (this->sdo_pin_ == nullptr)
    this->sdo_pin_ = io_bus::NULL_PIN;
  if (this->sdi_pin_ == nullptr)
    this->sdi_pin_ = io_bus::NULL_PIN;
  if (this->clk_pin_ == nullptr) {
    ESP_LOGE(TAG, "No clock pin for SPI");
    this->mark_failed();
    return;
  }

  if (this->using_hw_) {
    this->spi_bus_ =
        SPIComponent::get_bus(this->interface_, this->clk_pin_, this->sdo_pin_, this->sdi_pin_, this->data_pins_);
    if (this->spi_bus_ == nullptr) {
      ESP_LOGE(TAG, "Unable to allocate SPI interface");
      this->mark_failed();
    }
  } else {
    this->spi_bus_ = new SPIBus(this->clk_pin_, this->sdo_pin_, this->sdi_pin_);  // NOLINT
    this->clk_pin_->setup();
    this->clk_pin_->digital_write(true);
    this->sdo_pin_->setup();
    this->sdi_pin_->setup();
  }
}

void SPIByteBus::write_cmd_data(int cmd, const uint8_t *data, size_t length) {
  ESP_LOGV(TAG, "Write cmd %X, length %d", cmd, (unsigned) length);
  this->begin_transaction();
  if (cmd != -1) {
    this->dc_pin_->digital_write(false);
    this->client_->write_byte(cmd);
  }
  if (length != 0) {
    this->dc_pin_->digital_write(true);
    this->write_array(data, length);
  }
  // note - if there is no data phase, the transaction is ended with DC still in control state, but the
  // function must return with DC set to data state.
  this->end_transaction();
  this->dc_pin_->digital_write(true);
}

void SPIByteBus::dump_config() {
  ESP_LOGCONFIG(TAG, "  SPI Mode: %u", (unsigned) this->client_->mode_);
  ESP_LOGCONFIG(TAG, "  Data rate: %dMHz", (unsigned) (this->client_->data_rate_ / 1000000));
  LOG_PIN("  CS Pin: ", this->client_->cs_);
  LOG_PIN("  DC Pin: ", this->dc_pin_);
}

void SPIComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "SPI bus:");
  LOG_PIN("  CLK Pin: ", this->clk_pin_)
  LOG_PIN("  SDI Pin: ", this->sdi_pin_)
  LOG_PIN("  SDO Pin: ", this->sdo_pin_)
  for (size_t i = 0; i != this->data_pins_.size(); i++) {
    ESP_LOGCONFIG(TAG, "  Data pin %u: GPIO%d", i, this->data_pins_[i]);
  }
  if (this->spi_bus_->is_hw()) {
    ESP_LOGCONFIG(TAG, "  Using HW SPI: %s", this->interface_name_);
  } else {
    ESP_LOGCONFIG(TAG, "  Using software SPI");
  }
}

void SPIDelegateDummy::begin_transaction() { ESP_LOGE(TAG, "SPIDevice not initialised - did you call spi_setup()?"); }

uint8_t SPIDelegateBitBash::transfer(uint8_t data) { return this->transfer_(data, 8); }

void SPIDelegateBitBash::write(uint16_t data, size_t num_bits) { this->transfer_(data, num_bits); }

uint16_t SPIDelegateBitBash::transfer_(uint16_t data, size_t num_bits) {
  // Clock starts out at idle level
  this->clk_pin_->digital_write(clock_polarity_);
  uint16_t out_data = 0;

  for (uint8_t i = 0; i != num_bits; i++) {
    uint8_t shift;
    if (bit_order_ == BIT_ORDER_MSB_FIRST) {
      shift = num_bits - 1 - i;
    } else {
      shift = i;
    }

    if (clock_phase_ == CLOCK_PHASE_LEADING) {
      // sampling on leading edge
      this->sdo_pin_->digital_write(data & (1 << shift));
      this->cycle_clock_();
      out_data |= uint16_t(this->sdi_pin_->digital_read()) << shift;
      this->clk_pin_->digital_write(!this->clock_polarity_);
      this->cycle_clock_();
      this->clk_pin_->digital_write(this->clock_polarity_);
    } else {
      // sampling on trailing edge
      this->cycle_clock_();
      this->clk_pin_->digital_write(!this->clock_polarity_);
      this->sdo_pin_->digital_write(data & (1 << shift));
      this->cycle_clock_();
      out_data |= uint16_t(this->sdi_pin_->digital_read()) << shift;
      this->clk_pin_->digital_write(this->clock_polarity_);
    }
  }
  App.feed_wdt();
  return out_data;
}

}  // namespace spi
}  // namespace esphome
