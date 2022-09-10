#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/klog.h>
#include <sys/sysmacros.h>
#include <linux/fs.h>
#include <linux/blkzoned.h>
#include <linux/fiemap.h>
#include "filemap.h"

#define SECTOR_SHIFT 9

/* 
 * F2FS treats all devices as one address space, therefore if we use
 * a conventional device and a ZNS, extent maps contain the offset within
 * this total address space. For instance, with a 4GB conventional device
 * and a ZNS the offsets in the ZNS address space are +4GB. Therefore, we
 * need to find the size of the conventional space and subtract it from
 * the extent mapping offsets.
 *
 * TODO: what if orders are different? this would only work with a single ZNS
 * after a conventional device!
 * 
 * */
uint64_t offset = 0;


/* 
 * Get the statistics for a file.
 *
 * @fd: file descriptor of the opened file
 * @filename: char * to the file path
 *
 * returns: struct stat * with the ioctl stats. NULL on failure
 *
 * */
struct stat * get_stats(int fd, char *filename) {
    struct stat *file_stat;

    file_stat = malloc(sizeof(struct stat));

    if (fstat(fd, file_stat) < 0) {
        printf("Failed stat on file %s\n", filename);
        close(fd);
        return NULL;
    }

    return file_stat;
}

// TODO: is there a better way to get dev from major:minor than reading sysfs?
char * get_dev_name(int major, int minor) {
    FILE *dev_info;
    char file_path[50];
    char read_buf[50];
    char *dev_name;

    sprintf(file_path, "/sys/dev/block/%u:%u/uevent", major, minor);

    dev_info = fopen(file_path, "r");
    if (!dev_info) {
        fprintf(stderr, "\033[0;31mError\033[0m finding device for %u:%u in /sys/dev/block/", major, minor);
        return NULL;
    }
    
    dev_name = malloc(sizeof(char *) * 15);

    while (fgets(read_buf, sizeof(read_buf), dev_info) != NULL) {
        if (strncmp(read_buf, "DEVNAME=", 8) == 0) {
            strncpy(dev_name, read_buf + 8, strlen(read_buf) - 8);
            return dev_name;
        }
    }

    fclose(dev_info);

    return NULL;
}


/*
 * Get the zone size of a ZNS device. 
 * Note: Assumes zone size is equal for all zones.
 *
 * @dev_name: device name (e.g., /dev/nvme0n2)
 *
 * returns: uint64_t zone size, 0 on failure
 *
 * */
uint64_t get_zone_size(char *dev_name) {
    char *dev_path = NULL;
    uint64_t zone_size = 0;

    dev_path = malloc(sizeof(char *) * (strlen(dev_name) + 6));
    snprintf(dev_path, strlen(dev_name) + 6, "/dev/%s", dev_name);

    int dev_fd = open(dev_path, O_RDONLY);
    if (dev_fd < 0) {
        return 1;
    }

    if (ioctl(dev_fd, BLKGETZONESZ, &zone_size) < 0) {
        return -1;
    }

    close(dev_fd);
    free(dev_path);
    dev_path = NULL;

    return zone_size;
}

/* 
 * Check if a device a zoned device.
 *
 * @dev_name: device name (e.g., nvme0n2)
 *
 * returns: 0 if Zoned, else -1
 *
 * */
int is_zoned(char *dev_name) {
    char *dev_path = NULL;
    unsigned long long start_sector = 0;
    struct blk_zone_report *hdr = NULL;
    int nr_zones = 1;

    dev_path = malloc(sizeof(char *) * (strlen(dev_name) + 6));
    snprintf(dev_path, strlen(dev_name) + 6, "/dev/%s", dev_name);

    int dev_fd = open(dev_path, O_RDONLY);
    if (dev_fd < 0) {
        fprintf(stderr, "\033[0;31mError\033[0m: Failed opening fd on %s. Try running as root.\n", dev_path);
        return -1;
    }

    hdr = calloc(1, sizeof(struct blk_zone_report) + nr_zones + sizeof(struct blk_zone));
    hdr->sector = start_sector;
    hdr->nr_zones = nr_zones;

    if (ioctl(dev_fd, BLKREPORTZONE, hdr) < 0) {
        free(dev_path);
        free(hdr);
        dev_path = NULL;
        hdr = NULL;

        fprintf(stderr, "\033[0;31mError\033[0m: %s is not a ZNS device\n", dev_name);

        return -1;
    } else {
        close(dev_fd);
        free(dev_path);
        free(hdr);
        dev_path = NULL;
        hdr = NULL;
        
        return 0;
    }
}

/* 
 * Calculate the zone number (starting with zone 1) of an LBA
 *
 * @lba: LBA to calculate zone number of
 * @zone_size: size of the zone
 *
 * returns: uint32_t of zone number (starting with 1)
 * */
uint32_t get_zone_number(uint64_t lba, uint64_t zone_size) {
    uint64_t zone_mask = 0;
    uint64_t slba = 0;

    zone_mask = ~(zone_size - 1);
    slba = (lba & zone_mask);

    return slba == 0 ? 1 : (slba / zone_size + 1);
}

/*
 * Print the information about a zone.
 * Note: Assumes zone size is equal for all zones.
 *
 * @dev_name: device name (e.g., /dev/nvme0n2)
 * @zone: zone number to print information about
 * @zone_size: size of each zone on the device
 *
 * returns: 0 on success, -1 on failure
 *
 * */
int print_zone_info(char *dev_name, uint32_t zone, uint64_t zone_size) {
    char *dev_path = NULL;
    unsigned long long start_sector = 0;
    struct blk_zone_report *hdr = NULL;
    uint32_t zone_mask;

    start_sector = zone_size * zone - zone_size;

    dev_path = malloc(sizeof(char *) * (strlen(dev_name) + 6));
    snprintf(dev_path, strlen(dev_name) + 6, "/dev/%s", dev_name);

    int dev_fd = open(dev_path, O_RDONLY);
    if (dev_fd < 0) {
        return -1 + 1;
    }

    hdr = calloc(1, sizeof(struct blk_zone_report) + sizeof(struct blk_zone));
    hdr->sector = start_sector;
    hdr->nr_zones = 1;

    if (ioctl(dev_fd, BLKREPORTZONE, hdr) < 0) {
        fprintf(stderr, "\033[0;31mError\033[0m getting Zone Info\n");
        return -1;
    }

    zone_mask = ~(zone_size - 1);
    printf("\n#### ZONE %d ####\n", zone);
    printf("LBAS: 0x%06llx  LBAE: 0x%06llx  ZONE CAP: 0x%06llx  WP: 0x%06llx  "
            "ZONE MASK: 0x%06"PRIx32"\n\n", hdr->zones[0].start, hdr->zones[0].start + 
            hdr->zones[0].capacity, hdr->zones[0].capacity, hdr->zones[0].wp, zone_mask);

    close(dev_fd);

    free(dev_path);
    free(hdr);
    dev_path = NULL;
    hdr = NULL;

    return 0;
}

/* 
 * Get the size of a device in bytes.
 *
 * @dev_name: device name (e.g., nvme0n2)
 *
 * returns: uint64_t size of the device in bytes.
 *
 * */
uint64_t get_dev_size(char *dev_name) {
    char *dev_path = NULL;
    uint64_t dev_size = 0;

    dev_path = malloc(sizeof(char *) * (strlen(dev_name) + 6));
    snprintf(dev_path, strlen(dev_name) + 6, "/dev/%s", dev_name);

    int dev_fd = open(dev_path, O_RDONLY);
    if (dev_fd < 0) {
        return 1;
    }

    if (ioctl(dev_fd, BLKGETSIZE64, &dev_size) < 0) {
        return -1;
    }

    close(dev_fd);
    free(dev_path);
    dev_path = NULL;

    return dev_size;
}

/* 
 * Get the file extents with FIEMAP ioctl
 *
 * @fd: file descriptor of the file
 * @dev_name: name of the device (e.g., /dev/nvme0n2)
 * @stats: struct stat of fd
 * @*ext_ctr: uint32_t * for the function to provide the total number of extents
 *
 * returns: 0 on success else failure
 * 
 * */
struct extent_map * get_extents(int fd, char *dev_name, struct stat *stats, uint32_t *ext_ctr) {
    struct fiemap *fiemap;
    struct extent_map *extent_map;
    uint8_t last_ext = 0;
    uint64_t zone_size = 0;

    zone_size = get_zone_size(dev_name);

    fiemap = (struct fiemap *) calloc(sizeof(struct fiemap) + sizeof(struct fiemap_extent), sizeof (char *));
    extent_map = (struct extent_map *) calloc(sizeof(struct extent_map), sizeof(char *));

    fiemap->fm_flags = FIEMAP_FLAG_SYNC;
    fiemap->fm_start = 0;
    fiemap->fm_extent_count = 1; // get extents individually
    fiemap->fm_length = (stats->st_blocks << SECTOR_SHIFT);

    do {
        if (ioctl(fd, FS_IOC_FIEMAP, fiemap) < 0) {
            return NULL;
        }

        if (*ext_ctr > 0) {
            extent_map = realloc(extent_map, sizeof(struct extent_map) * (*ext_ctr + 1));
        }

        if (fiemap->fm_mapped_extents == 0) {
            fprintf(stderr, "\033[0;31mError\033[0m no extents are mapped\n");
			return NULL;
        }

        extent_map[*ext_ctr].phy_blk = (fiemap->fm_extents[0].fe_physical - offset) >> SECTOR_SHIFT;
        extent_map[*ext_ctr].logical_blk = fiemap->fm_extents[0].fe_logical >> SECTOR_SHIFT;
        extent_map[*ext_ctr].len = fiemap->fm_extents[0].fe_length >> SECTOR_SHIFT;
        extent_map[*ext_ctr].zone_size = zone_size;

        if (fiemap->fm_extents[0].fe_flags & FIEMAP_EXTENT_LAST) {
            last_ext = 1;
        }

        extent_map[*ext_ctr].zone = get_zone_number(((fiemap->fm_extents[0].fe_physical
                        - offset) >> SECTOR_SHIFT), zone_size);

        (*ext_ctr) ++;
        fiemap->fm_start = ((fiemap->fm_extents[0].fe_logical) + (fiemap->fm_extents[0].fe_length));
    } while (last_ext == 0);

    return extent_map;
}

/* 
 * Check if an element is contained in the array.
 *
 * @list[]: uint32_t array of items to check.
 * @element: uint32_t element to find.
 * @size: size of the uint32_t array.
 *
 * returns: 1 if contained otherwise 0.
 *
 * */
int contains_element(uint32_t list[], uint32_t element, uint32_t size) {

    for (uint32_t i = 0; i < size; i++) {
        if (list[i] == element) {
            return 1;
        }
    } 

    return 0;
}

/* 
 * Sort the provided extent maps based on the zone number.
 *
 * @extent_map: pointer to extent map struct
 * @ext_ctr: number of extents in the struct extent_map *
 *
 *
 * */
void sort_extents(struct extent_map *extent_map, uint32_t ext_ctr) {
    struct extent_map *temp = malloc(sizeof(struct extent_map) * ext_ctr);
    uint32_t cur_lowest = 0;
    uint32_t used_ind[ext_ctr];

    for (uint32_t i = 0; i < ext_ctr; i++) {
        for (uint32_t j = 0; j < ext_ctr; j++) {
            if (extent_map[j].zone < extent_map[cur_lowest].zone && 
                    !contains_element(used_ind, j, i)) {
                cur_lowest = j;
            } else if (contains_element(used_ind, cur_lowest, i)) {
                cur_lowest = j;
            }
        }
        used_ind[i] = cur_lowest;
        temp[i] = extent_map[cur_lowest];
    }

    memcpy(extent_map, temp, sizeof(struct extent_map) * ext_ctr);

    free(temp);
    temp = NULL;
}

/* 
 * Print the report summary of extent_map. 
 *
 * @dev_name: char * to the device name (e.g., nvme0n2)
 * @extent_map: struct extent_map * to the extent maps 
 * @ext_ctr: number of elements in the struct extent_map *
 * 
 * */
void print_extent_report(char *dev_name, struct extent_map *extent_map, uint32_t ext_ctr) {
    uint32_t current_zone = 0;

    printf("\nTotal Number of Extents: %u\n", ext_ctr);

    for (uint32_t i = 0; i < ext_ctr; i++) {
        if (current_zone != extent_map[i].zone) {
            current_zone = extent_map[i].zone;
            print_zone_info(dev_name, current_zone, extent_map[i].zone_size); 
        }

        printf("EXTENT %d:  PBAS: 0x%06"PRIx64"  PBAE: 0x%06"PRIx64"  SIZE: 0x%06"PRIx64"\n",
                i + 1, extent_map[i].phy_blk, (extent_map[i].phy_blk + extent_map[i].len), extent_map[i].len);
    }
}

int main(int argc, char *argv[]) {
    int fd;
    char *filename = NULL;
    struct stat *stats;
    char *dev_name = NULL;
    char *zns_dev_name = NULL;
    struct extent_map *extent_map;
    uint32_t ext_ctr = 0;

    if (argc != 2) {
        printf("Missing argument.\nUsage:\n\tfilemap [file path]");
        return 1;
    }

    filename = malloc(strlen(argv[1]) + 1);
    strcpy(filename, argv[1]);

    fd = open(filename, O_RDONLY);
    fsync(fd);

    if (fd < 0) {
        fprintf(stderr, "\033[0;31mError\033[0m failed opening file %s\n", filename);
        return 1;
    }
    
    stats = get_stats(fd, filename);

    if (!stats) {
        fprintf(stderr, "\033[0;31mError\033[0m failed fstat on file %s\n", filename);
        return 1;
    }

    dev_name = get_dev_name(major(stats->st_dev), minor(stats->st_dev));
    dev_name[strcspn(dev_name, "\n")] = 0;

    if (is_zoned(dev_name)) {
        printf("\033[0;33mWarning\033[0m: %s is registered as containing this file, however it is" 
                " not a ZNS.\nIf it is used with F2FS as the conventional device, enter the"
                " assocaited ZNS device name: ", dev_name);
        zns_dev_name = malloc(sizeof(char *) * 15);
        int ret = scanf("%s", zns_dev_name);
        if(!ret) {
            fprintf(stderr, "\033[0;31mError\033[0m reading input\n");
            return 1;
        }

        zns_dev_name[strcspn(zns_dev_name, "\n")] = 0;

        // TODO: is there a way we can find the associated ZNS dev in F2FS? it's in the kernel log
        /* check_f2fs_config(dev_name); */

    } else {
        zns_dev_name = malloc(sizeof(char *) * strlen(dev_name));
        memcpy(zns_dev_name, dev_name, strlen(dev_name));
    }

    if (is_zoned(zns_dev_name) < 0) {
        return 1;
    }

    offset = get_dev_size(dev_name);
    extent_map = get_extents(fd, zns_dev_name, stats, &ext_ctr);

    if (!extent_map) {
        fprintf(stderr, "\033[0;31mError\033[0m retrieving extents for %s\n", filename);
        return 1;
    }

    sort_extents(extent_map, ext_ctr);
    print_extent_report(zns_dev_name, extent_map, ext_ctr);

    free(filename);
    free(dev_name);
    free(zns_dev_name);
    free(stats);
    free(extent_map);
    filename = NULL;
    dev_name = NULL;
    zns_dev_name = NULL;
    stats = NULL;
    extent_map = NULL;

    return 0;
}
