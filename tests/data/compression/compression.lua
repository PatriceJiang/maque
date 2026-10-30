function on_start()
    if zstd.available() == 0 or zlib.available() == 0 then
        log("compression_e2e unavailable")
        quit()
        return
    end

    local parts = {}
    for i = 1, 2048 do parts[i] = "abc\0" end
    local raw = table.concat(parts) .. string.char(255, 128, 0, 127)
    local zs = zstd.compress(raw, 3)
    local zr = zstd.decompress(zs)
    local zl = zlib.compress(raw, 6)
    local zlr = zlib.decompress(zl)
    local ok = #zs > 0 and #zl > 0 and #zs < #raw and #zl < #raw and zr == raw and zlr == raw
    log("compression_e2e " .. (ok and "ok" or "FAIL") .. " lang=lua zstd=" .. #zs .. " zlib=" .. #zl)
    quit()
end

function update() end
