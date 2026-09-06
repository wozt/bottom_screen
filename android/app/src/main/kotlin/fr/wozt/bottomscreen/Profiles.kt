package fr.wozt.bottomscreen

import android.content.SharedPreferences
import org.json.JSONArray
import org.json.JSONObject

/**
 * The servers this phone knows about.
 *
 * Three emulators can run at once, each on its own port, and the port is
 * not even fixed -- a server whose port is taken walks upwards and
 * announces where it landed. Retyping an address and a port to move from
 * the DS to the Wii U is the kind of small friction that gets in the way
 * every single time, so the ones that worked are kept.
 *
 * Stored as JSON in the same preferences as everything else. A list this
 * short does not need a database, and a database would need a schema to
 * migrate the first time a field is added.
 */
data class Profile(val name: String, val host: String, val port: Int) {
    val where: String get() = "$host:$port"
}

object Profiles {

    private const val KEY = "profiles"

    fun load(prefs: SharedPreferences): List<Profile> {
        val raw = prefs.getString(KEY, null) ?: return emptyList()
        return try {
            val arr = JSONArray(raw)
            (0 until arr.length()).mapNotNull { i ->
                val o = arr.optJSONObject(i) ?: return@mapNotNull null
                val host = o.optString("host")
                if (host.isEmpty()) null
                else Profile(
                    name = o.optString("name").ifEmpty { host },
                    host = host,
                    port = o.optInt("port", BsProtocol.DEFAULT_PORT)
                )
            }
        } catch (e: Exception) {
            /* Nothing here is worth losing a launch over: a list that
             * cannot be read is treated as an empty one. */
            emptyList()
        }
    }

    fun save(prefs: SharedPreferences, list: List<Profile>) {
        val arr = JSONArray()
        for (p in list) {
            arr.put(JSONObject().apply {
                put("name", p.name)
                put("host", p.host)
                put("port", p.port)
            })
        }
        prefs.edit().putString(KEY, arr.toString()).apply()
    }

    /*
     * Saving is keyed on the address, not the name. Two entries pointing
     * at the same server are never what someone meant -- they wanted to
     * rename the one they had.
     */
    fun upsert(prefs: SharedPreferences, p: Profile): List<Profile> {
        val list = load(prefs).filterNot { it.host == p.host && it.port == p.port }
        val next = list + p
        save(prefs, next)
        return next
    }

    fun remove(prefs: SharedPreferences, p: Profile): List<Profile> {
        val next = load(prefs).filterNot { it.host == p.host && it.port == p.port }
        save(prefs, next)
        return next
    }

    /*
     * What to call a server nobody has named yet.
     *
     * The console is only known once it has answered, so this is offered
     * as a suggestion after a connection rather than demanded before
     * one -- and two DS servers on one machine still need telling apart,
     * hence the port.
     */
    fun suggestName(consoleLabel: String?, host: String, port: Int): String =
        if (consoleLabel.isNullOrEmpty()) "$host:$port" else "$consoleLabel ($port)"
}
