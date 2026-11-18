package com.novikov.piplink

/** One field of a Pip Link line: no pipe, no newlines, at most [n] characters. */
object Protocol {
    fun clean(s: String?, n: Int): String {
        val t = (s ?: "").replace("|", "/").replace("\r", " ").replace("\n", " ")
        val one = t.split(Regex("\\s+")).filter { it.isNotEmpty() }.joinToString(" ")
        return if (one.length > n) one.substring(0, n) else one
    }
}
