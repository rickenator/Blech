# On-device model integration

The firmware already contains the PLE inference runtime and reserves the
`model` flash partition for a local model. It intentionally ships without the
old TinyStories assets.

The product model is defined in `../esp32-ai/DIALOGUE_MODEL.md`: a 4K-token,
256-context dialogue model trained for short replies and two read-only tools.
`Auto` mode tries the LAN agent first and uses this model when the LAN backend is
unavailable. `Local` forces on-device inference.

After training and acceptance testing:

```sh
tools/install-model-assets.sh ../esp32-ai
tools/build-local.sh
idf.py -p /dev/ttyACM0 flash
esptool.py --chip esp32s3 --port /dev/ttyACM0 \
  write_flash 0x150000 model/model.bin
```

The generated `vocab.h` is compiled into the app. `model.bin` is flashed
separately at the partition offset shown in `partitions.csv`. Never flash a model
larger than the partition.
