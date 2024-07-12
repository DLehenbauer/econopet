#include "sd.h"
#include "../hw.h"

#include "sd_card.h"
#include "diskio.h"
#include "f_util.h"
#include "ff.h"
#include "hw_config.h"
#include "rtc.h"

// Hardware Configuration of SPI "objects"
// Note: multiple SD cards can be driven by one SPI if they use different slave
// selects.
static spi_t spis[] = { // One for each SPI.
    {
        .hw_inst = SD_SPI_INSTANCE,
        .miso_gpio = SD_DAT_GP,
        .mosi_gpio = SD_CMD_GP,
        .sck_gpio = SD_CLK_GP,
        .baud_rate = SPI_MHZ * 1000 * 1000
    }
};

// Hardware Configuration of the SD Card "objects"
static sd_card_t sd_cards[] = { // One for each SD card
    {
        .pcName = "0:",             // Name used to mount device
        .spi = &spis[0],            // Pointer to the SPI driving this card
        .ss_gpio = SD_CSN_GP,       // The SPI slave select GPIO for this SD card
        .use_card_detect = true,
        .card_detect_gpio = SD_DETECT,
        .card_detected_true = 0     // SD_DETECT pin is low when card inserted
    }
};

size_t sd_get_num() { return count_of(sd_cards); }

sd_card_t* sd_get_by_num(size_t num) {
    if (num <= sd_get_num()) {
        return &sd_cards[num];
    } else {
        return NULL;
    }
}

size_t spi_get_num() { return count_of(spis); }

spi_t* spi_get_by_num(size_t num) {
    if (num <= spi_get_num()) {
        return &spis[num];
    } else {
        return NULL;
    }
}

sd_card_t* pSD;

void sd_init() {
    time_init();

    puts("Hello, world!");

    // See FatFs - Generic FAT Filesystem Module, "Application Interface",
    // http://elm-chan.org/fsw/ff/00index_e.html
    pSD = sd_get_by_num(0);

    // PicoDVI uses DMA_IRQ_0 (exclusive), SD uses DMA_IRQ_1 (shared).
    set_spi_dma_irq_channel(/* useChannel1: */ true, /* shared: */ true);

    FRESULT fr = f_mount(&pSD->fatfs, pSD->pcName, 1);
    if (FR_OK != fr) {
        panic("f_mount error: %s (%d)\n", FRESULT_str(fr), fr);
    }
}

void sd_read_file(char* filename) {
    FIL fil;
    FRESULT fr = f_open(&fil, filename, FA_READ);
    if (FR_OK != fr) {
        printf("f_open error: %s (%d)\n", FRESULT_str(fr), fr);
        return;
    }

    char buf[256];
    
    while (f_gets(buf, sizeof buf, &fil)) {
        printf("%s", buf);
    }
    
    fr = f_close(&fil);
    if (FR_OK != fr) {
        printf("f_close error: %s (%d)\n", FRESULT_str(fr), fr);
    }
}
