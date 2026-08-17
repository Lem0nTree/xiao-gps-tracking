package com.example.xiaogpstracker

import android.content.Context
import java.io.File
import java.util.Locale
import java.util.TreeMap

class TrackStore(context: Context) {
    private val file = File(context.filesDir, "track.bin")
    private val recordsBySeq = TreeMap<Long, GpsRecord>()

    init {
        load()
    }

    val records: List<GpsRecord>
        get() = recordsBySeq.values.toList()

    val lastSeq: Long
        get() = recordsBySeq.lastKeyOrNull() ?: 0L

    fun merge(newRecords: Collection<GpsRecord>) {
        newRecords.forEach { recordsBySeq[it.seq] = it }
        persist()
    }

    fun clear() {
        recordsBySeq.clear()
        if (file.exists()) file.delete()
    }

    fun sampleForMap(maxPoints: Int = 5000): List<GpsRecord> {
        val all = records
        if (all.size <= maxPoints) return all

        val step = all.size.toDouble() / maxPoints.toDouble()
        val sampled = ArrayList<GpsRecord>(maxPoints + 1)
        var i = 0.0
        while (i < all.size) {
            sampled += all[i.toInt().coerceAtMost(all.lastIndex)]
            i += step
        }
        if (sampled.lastOrNull()?.seq != all.last().seq) sampled += all.last()
        return sampled
    }

    fun csv(): String = buildString {
        appendLine("sequence,timestamp_utc,latitude,longitude,hdop,satellites")
        recordsBySeq.values.forEach { r ->
            append(r.seq).append(',')
            append(r.utcIso()).append(',')
            append(String.format(Locale.US, "%.7f", r.latitude)).append(',')
            append(String.format(Locale.US, "%.7f", r.longitude)).append(',')
            append(String.format(Locale.US, "%.2f", r.hdop)).append(',')
            append(r.satellites).appendLine()
        }
    }

    fun gpx(): String = buildString {
        appendLine("""<?xml version="1.0" encoding="UTF-8"?>""")
        appendLine("""<gpx version="1.1" creator="XIAO GPS Tracker" xmlns="http://www.topografix.com/GPX/1/1">""")
        appendLine("  <trk>")
        appendLine("    <name>XIAO GPS track</name>")
        appendLine("    <trkseg>")
        recordsBySeq.values.forEach { r ->
            append("      <trkpt lat=\"")
            append(String.format(Locale.US, "%.7f", r.latitude))
            append("\" lon=\"")
            append(String.format(Locale.US, "%.7f", r.longitude))
            appendLine("\">")
            append("        <time>").append(r.utcIso()).appendLine("</time>")
            if (r.hdopX100 > 0) {
                append("        <hdop>")
                    .append(String.format(Locale.US, "%.2f", r.hdop))
                    .appendLine("</hdop>")
            }
            if (r.satellites > 0) {
                append("        <sat>").append(r.satellites).appendLine("</sat>")
            }
            appendLine("      </trkpt>")
        }
        appendLine("    </trkseg>")
        appendLine("  </trk>")
        appendLine("</gpx>")
    }

    private fun load() {
        if (!file.exists()) return
        val bytes = file.readBytes()
        var offset = 0
        while (offset + GpsRecord.SIZE <= bytes.size) {
            GpsRecord.fromDeviceBytes(bytes, offset)?.let { recordsBySeq[it.seq] = it }
            offset += GpsRecord.SIZE
        }
    }

    private fun persist() {
        file.outputStream().buffered().use { out ->
            recordsBySeq.values.forEach { out.write(it.toDeviceBytes()) }
        }
    }

    private fun <K, V> TreeMap<K, V>.lastKeyOrNull(): K? =
        if (isEmpty()) null else lastKey()
}
