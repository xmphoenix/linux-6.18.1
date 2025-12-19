#!/bin/bash

ELF_FILE="vmlinux"

get_section_address_objdump() {
    local section=$1
    local address=$(objdump -h "$ELF_FILE" | awk -v section="$section" '$2 == section {print $4}')
    # 转换为十六进制数，加上 0x40200000，然后减去 0x8000000，只保留低8位
    local modified_address=$(printf "0x%x" $(( (0x$address + 0x40200000 - 0x8000000) & 0xffffffff )))
    echo "$modified_address"
}

sections=(".head.text" ".text" ".rodata" ".init.text" ".rodata.text" ".init.data")

# 获取各个段的入口地址
addresses=()
for section in "${sections[@]}"; do
    address=$(get_section_address_objdump "$section")
    addresses+=("$address")
done

# 输出结果
echo "add-symbol-file $ELF_FILE -s ${sections[0]} ${addresses[0]} -s ${sections[1]} ${addresses[1]} -s ${sections[2]} ${addresses[2]} -s ${sections[3]} ${addresses[3]} -s ${sections[4]} ${addresses[4]} -s ${sections[5]} ${addresses[5]}"

