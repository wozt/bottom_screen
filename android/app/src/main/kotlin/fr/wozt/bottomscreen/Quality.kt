package fr.wozt.bottomscreen

/**
 * Bitrate presets.
 *
 * These screens are small enough that even the top setting is modest by
 * streaming standards -- a Wii U GamePad frame is 854x480, and a DS one
 * is smaller than most thumbnails. The reason to offer a choice at all
 * is the link, not the picture: a busy Wi-Fi network is where a lower
 * setting earns its keep.
 *
 * AUTO leaves it to the server, which derives a bitrate from the
 * resolution. That is the right default -- it already knows the size,
 * and it is the one that changes per console.
 */
enum class Quality(val label: String, val bitrate: Int) {
    AUTO("Automatic", 0),
    LOW("Low  ~400 kbit/s", 400_000),
    MEDIUM("Medium  ~1 Mbit/s", 1_000_000),
    HIGH("High  ~2.5 Mbit/s", 2_500_000),
    MAX("Maximum  ~6 Mbit/s", 6_000_000);

    companion object {
        fun byName(name: String?): Quality =
            entries.firstOrNull { it.name == name } ?: AUTO
    }
}
