#! /bin/bash

TRACETIME=30 # time to trace in sec

set -e

if [[ "$#" -ne "1" ]]; then echo "Requires ZNS device name (e.g. nvme2n2) argument" && exit; fi

DEV=$1

if [[ "$(cat /sys/block/$DEV/queue/zoned)" != "host-managed" ]]; then
    echo "${DEV} is not a zoned device."
    exit 1
fi

ZONE_SIZE=$(sudo env "PATH=${PATH}" nvme zns report-zones /dev/${DEV} -d 2 | tail -n1 | grep -o 'SLBA:.*$' | awk '{print strtonum($2)}')
NR_ZONES=$(sudo env "PATH=$PATH" nvme zns report-zones /dev/${DEV} -d 1 | grep 'nr_zones' | awk '{print $2}')

# TODO: can we automate finding this? or specify it in args?
MNT="/mnt/f2fs"

echo "Tracing ${DEV}"
echo "Hit Ctrl-C or send INT to stop trace and generate plots"

# TODO: we want a new dir for each trace, use this in the generation
DATA_DIR=${DEV}-$(date +"%Y_%m_%d_%I_%M_%p")
mkdir -p ${DATA_DIR}
sudo chown -R ${USER} ${DATA_DIR}

# ensure that the inode script is the longest running script, past setup times of high memory usage probes
INODE_TRACETIME=$(echo "$TRACETIME + 20" | bc)

# Update the tracetime in the files
sed -i "s/interval:s:[0-9]\+/interval:s:${TRACETIME}/g" nvme-probes.bt rocksdb-probes.bt vfs-probes.bt mm-probes.bt f2fs-probes.bt
sed -i "s/interval:s:[0-9]\+/interval:s:${INODE_TRACETIME}/g" inode-probes.bt

# TODO: lookup bpftrace install path and use it
echo "Inserting NVMe Probes"
(sudo env "BPFTRACE_MAP_KEYS_MAX=262144" bpftrace ./nvme-probes.bt ${DEV} ${ZONE_SIZE} -o ${DATA_DIR}/nvme_data.json -f json) &
echo "Inserting F2FS Probes"
(sudo env "BPFTRACE_MAP_KEYS_MAX=262144" bpftrace -I include/f2fs.h ./f2fs-probes.bt ${ZONE_SIZE} -o ${DATA_DIR}/f2fs_data.json -f json) &

echo "Inserting VFS Probes"
(sudo env "BPFTRACE_MAP_KEYS_MAX=32768" bpftrace ./vfs-probes.bt -o ${DATA_DIR}/vfs_data.json -f json) &
echo "Inserting MM Probes"
(sudo env "BPFTRACE_MAP_KEYS_MAX=32768" bpftrace ./mm-probes.bt -o ${DATA_DIR}/mm_data.json -f json) &
echo "Inserting RocksDB Probes"
(sudo env "BPFTRACE_MAP_KEYS_MAX=4096" bpftrace ./rocksdb-probes.bt -o ${DATA_DIR}/rocksdb.json -f json) &
echo "Inserting inode Trace Probes"
(sudo env "BPFTRACE_MAP_KEYS_MAX=4096" bpftrace -I include/f2fs.h ./inode-probes.bt -o ${DATA_DIR}/inodes.json -f json) &

printf "\nTracing for ${TRACETIME} seconds\n"

wait

read -r "Input comments to embed in data file (Enter for none): " COMMENTS

echo ${COMMENTS} > ${DATA_DIR}/README.md

echo "\nGenerating trace timeline\n"
python3 tracegen.py -d ${DATA_DIR}