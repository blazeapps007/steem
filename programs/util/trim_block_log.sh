#!/bin/bash

create_backup=true
target_size=1048576000

blocks_file="block_log"
offset_file="block_log.offset"
backup_file="block_log.org"

if [[ ! -f "$blocks_file" ]]; then
   echo "Error: $blocks_file does not exist."
   exit 1
fi

current_size=$(stat -c%s "$blocks_file")

if (( current_size <= target_size )); then
   echo "No trimming needed. Current size ($current_size bytes) is <= desired size ($target_size bytes)."
   exit 0
fi

bytes_to_cut=$((current_size - target_size))
new_offset=$bytes_to_cut

if [[ -f "$offset_file" ]]; then
   old_offset=$(<"$offset_file")

   if [[ "$old_offset" =~ ^[0-9]+$ ]]; then
      new_offset=$((new_offset + old_offset))
   else
      echo "Error: $offset_file does not contain a valid number."
      exit 1
   fi
fi

echo "$new_offset" > "$offset_file"
tail -c "$target_size" "$blocks_file" > "$blocks_file.tmp" || exit $?

if $create_backup; then
   mv -f "$blocks_file" "$backup_file" || exit $?
fi

mv "$blocks_file.tmp" "$blocks_file" || exit $?
echo "Trimmed $bytes_to_cut bytes from $blocks_file. New size is $(stat -c%s "$blocks_file") bytes."
