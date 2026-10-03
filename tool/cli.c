/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * meusbpca: image ATA PC Cards through the Toshiba MEUSBPCA adapter.
 * The adapter must be bound to WinUSB.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/datafab.h"
#include "../core/version.h"
#include "../service/winusb_io.h"

static void usage(void)
{
    fputs("Usage:\n"
          "  meusbpca probe                 Show the USB descriptors of the adapter.\n"
          "  meusbpca info                  Identify the inserted card.\n"
          "  meusbpca read <image>          Save the whole card to an image file.\n"
          "  meusbpca write <image> --yes   Overwrite the card from an image file.\n", stderr);
}

static void file_error(const char *action, const char *path)
{
    fprintf(stderr, "The file %s could not be %s: %s.\n", path, action, strerror(errno));
}

static int open_adapter(wu_dev *usb)
{
    DWORD error = wu_open(usb);

    if (error == ERROR_NOT_FOUND)
        fputs("The MEUSBPCA adapter (USB ID 07C4:A006) was not found. "
              "Check that it is connected and bound to the WinUSB driver.\n", stderr);
    else if (error)
        fprintf(stderr, "The MEUSBPCA adapter could not be opened (Windows error %lu). "
                "Check that it is bound to the WinUSB driver.\n", error);
    return error ? -1 : 0;
}

static int open_card(wu_dev *usb, df_dev *card)
{
    df_io io;

    if (open_adapter(usb) != 0)
        return -1;
    wu_io(usb, &io);
    df_init(card, &io);
    if (df_identify(card) != DF_OK) {
        fputs("No ATA card was detected in the adapter. "
              "The card is missing or is not an ATA card.\n", stderr);
        wu_close(usb);
        return -1;
    }
    return 0;
}

static void progress(const char *action, df_u32 done, df_u32 total)
{
    static df_u32 shown = (df_u32)-1;
    df_u32 percent = (df_u32)((unsigned __int64)done * 100 / total);

    if (percent == shown)
        return;
    shown = percent;
    fprintf(stderr, "\r%s sector %lu of %lu (%lu%%)", action, done, total, percent);
}

static void print_string(wu_dev *usb, const char *label, UCHAR index)
{
    UCHAR raw[256];
    ULONG got = 0, i;

    if (!index || !WinUsb_GetDescriptor(usb->usb, USB_STRING_DESCRIPTOR_TYPE, index, 0x0409,
                                        raw, sizeof(raw), &got) || got < 2)
        return;
    printf("%-12s '", label);
    for (i = 2; i + 1 < got; i += 2)
        putchar(raw[i + 1] == 0 && raw[i] >= 0x20 && raw[i] < 0x7F ? raw[i] : '?');
    puts("'");
}

static int cmd_probe(void)
{
    static const char *const kinds[] = { "control", "isochronous", "bulk", "interrupt" };
    wu_dev usb;
    USB_DEVICE_DESCRIPTOR dd;
    UCHAR cfg[512];
    ULONG got = 0, pos;

    if (open_adapter(&usb) != 0)
        return 1;
    if (!WinUsb_GetDescriptor(usb.usb, USB_DEVICE_DESCRIPTOR_TYPE, 0, 0, (PUCHAR)&dd, sizeof(dd), &got) ||
        got < sizeof(dd)) {
        fprintf(stderr, "The USB device descriptor could not be read (Windows error %lu).\n", GetLastError());
        wu_close(&usb);
        return 1;
    }
    printf("device   %04x:%04x rev %04x\n", dd.idVendor, dd.idProduct, dd.bcdDevice);
    printf("usb      %04x\n", dd.bcdUSB);
    printf("class    %02x/%02x/%02x\n", dd.bDeviceClass, dd.bDeviceSubClass, dd.bDeviceProtocol);
    printf("ep0 size %u  configs %u\n", dd.bMaxPacketSize0, dd.bNumConfigurations);

    if (WinUsb_GetDescriptor(usb.usb, USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0, cfg, sizeof(cfg), &got)) {
        for (pos = 0; pos + 2 <= got && cfg[pos] >= 2 && pos + cfg[pos] <= got; pos += cfg[pos]) {
            const UCHAR *d = cfg + pos;

            if (d[1] == USB_CONFIGURATION_DESCRIPTOR_TYPE && d[0] >= 9)
                printf("\nconfig %u: %u interface(s), attrs %02x, %u mA\n", d[5], d[4], d[7], d[8] * 2);
            else if (d[1] == USB_INTERFACE_DESCRIPTOR_TYPE && d[0] >= 9)
                printf("  interface %u alt %u: class %02x/%02x/%02x, %u endpoint(s)\n",
                       d[2], d[3], d[5], d[6], d[7], d[4]);
            else if (d[1] == USB_ENDPOINT_DESCRIPTOR_TYPE && d[0] >= 7)
                printf("    ep %02x %s %-11s maxpacket %u interval %u\n", d[2],
                       (d[2] & 0x80) ? "IN " : "OUT", kinds[d[3] & 3], d[4] | (d[5] << 8), d[6]);
        }
    }
    putchar('\n');
    print_string(&usb, "manufacturer", dd.iManufacturer);
    print_string(&usb, "product", dd.iProduct);
    print_string(&usb, "serial", dd.iSerialNumber);
    wu_close(&usb);
    return 0;
}

static int cmd_info(void)
{
    wu_dev usb;
    df_dev card;
    char text[41];

    if (open_card(&usb, &card) != 0)
        return 1;
    printf("slot      %d\n", card.slot);
    df_ata_string(&card, 27, 20, text);
    printf("model     %s\n", text);
    df_ata_string(&card, 10, 10, text);
    printf("serial    %s\n", text);
    df_ata_string(&card, 23, 4, text);
    printf("firmware  %s\n", text);
    printf("geometry  %u cyl / %u heads / %u spt\n", df_ident_word(&card, 1),
           df_ident_word(&card, 3), df_ident_word(&card, 6));
    printf("capacity  %lu sectors (%.1f MB)\n", card.sectors, card.sectors * (double)DF_SECTOR_SIZE / 1e6);
    wu_close(&usb);
    return 0;
}

static int cmd_read(const char *path)
{
    static df_u8 chunk[DF_MAX_SECTORS * DF_SECTOR_SIZE];
    wu_dev usb;
    df_dev card;
    FILE *out;
    df_u32 lba = 0, n;
    int rc = 1;

    if (open_card(&usb, &card) != 0)
        return 1;
    out = fopen(path, "wb");
    if (!out) {
        file_error("created", path);
        wu_close(&usb);
        return 1;
    }
    while (lba < card.sectors) {
        n = card.sectors - lba > DF_MAX_SECTORS ? DF_MAX_SECTORS : card.sectors - lba;
        if (df_read(&card, lba, n, chunk) != DF_OK) {
            fprintf(stderr, "\nReading the card has failed at sector %lu.\n", lba);
            goto done;
        }
        if (fwrite(chunk, DF_SECTOR_SIZE, n, out) != n) {
            fputc('\n', stderr);
            file_error("written", path);
            goto done;
        }
        lba += n;
        progress("Reading", lba, card.sectors);
    }
    rc = 0;
done:
    if (fclose(out) != 0 && rc == 0) {
        fputc('\n', stderr);
        file_error("written", path);
        rc = 1;
    }
    if (rc == 0)
        fprintf(stderr, "\nThe card has been saved to %s (%lu sectors).\n", path, card.sectors);
    wu_close(&usb);
    return rc;
}

static int cmd_write(const char *path, int confirmed)
{
    static df_u8 chunk[DF_MAX_SECTORS * DF_SECTOR_SIZE];
    wu_dev usb;
    df_dev card;
    FILE *in;
    __int64 size;
    df_u32 total, lba = 0, n;
    int rc = 1;

    in = fopen(path, "rb");
    if (!in) {
        file_error("opened", path);
        return 1;
    }
    _fseeki64(in, 0, SEEK_END);
    size = _ftelli64(in);
    _fseeki64(in, 0, SEEK_SET);

    if (open_card(&usb, &card) != 0) {
        fclose(in);
        return 1;
    }
    if (size <= 0 || size % DF_SECTOR_SIZE || size > (__int64)card.sectors * DF_SECTOR_SIZE) {
        fprintf(stderr, "The image file %s is %I64d bytes, which cannot be written to this card. "
                "It must be a multiple of 512 bytes and no larger than the card (%I64d bytes).\n",
                path, size, (__int64)card.sectors * DF_SECTOR_SIZE);
        goto done;
    }
    if (!confirmed) {
        fputs("Writing will overwrite the contents of the card. "
              "Run the command again with --yes to proceed.\n", stderr);
        goto done;
    }
    total = (df_u32)(size / DF_SECTOR_SIZE);
    while (lba < total) {
        n = total - lba > DF_MAX_SECTORS ? DF_MAX_SECTORS : total - lba;
        if (fread(chunk, DF_SECTOR_SIZE, n, in) != n) {
            fputc('\n', stderr);
            file_error("read", path);
            goto done;
        }
        if (df_write(&card, lba, n, chunk) != DF_OK) {
            fprintf(stderr, "\nWriting the card has failed at sector %lu.\n", lba);
            goto done;
        }
        lba += n;
        progress("Writing", lba, total);
    }
    fprintf(stderr, "\nThe image %s has been written to the card (%lu sectors).\n", path, total);
    rc = 0;
done:
    fclose(in);
    wu_close(&usb);
    return rc;
}

int main(int argc, char **argv)
{
    fputs(MEUSBPCA_BANNER("Toshiba MEUSBPCA PC Card Imaging Tool"), stdout);
    fflush(stdout);

    if (argc == 2 && strcmp(argv[1], "probe") == 0)
        return cmd_probe();
    if (argc == 2 && strcmp(argv[1], "info") == 0)
        return cmd_info();
    if (argc == 3 && strcmp(argv[1], "read") == 0)
        return cmd_read(argv[2]);
    if (argc >= 3 && argc <= 4 && strcmp(argv[1], "write") == 0)
        return cmd_write(argv[2], argc == 4 && strcmp(argv[3], "--yes") == 0);
    usage();
    return 2;
}
