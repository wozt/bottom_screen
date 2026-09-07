package fr.wozt.bottomscreen

/**
 * Whether to draw the on-screen buttons.
 *
 * Three states rather than a checkbox, because the useful default is
 * neither on nor off: someone who plugs a controller in wants the
 * buttons to get out of the way without being asked, and someone who
 * unplugs it wants them back. A checkbox can express the two ends of
 * that but not the behaviour between them.
 */
enum class PadVisibility(val label: String) {
    AUTOMATIC("Automatic — hidden while a pad is connected"),
    ALWAYS("Always shown"),
    NEVER("Never shown");

    companion object {
        fun byName(name: String?): PadVisibility =
            entries.firstOrNull { it.name == name } ?: AUTOMATIC
    }
}
