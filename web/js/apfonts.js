// apfonts.js - the font files of an AP (GET /api/fonts, #177): fonts the AP draws with besides
// its built-in ones, which layouts name like a built-in font. The renderer loads the same files
// (renderer.setFonts) so a layout that names one is drawn here as the AP draws it.
//
//   const fonts = apFonts();               one per page: it keeps the files it has read
//   const { supported, info, files } = await fonts.read(client)
//       info: what GET /api/fonts says ({ max_fonts, max_bytes, bytes, builtin, fonts }),
//       files: { name: text of the font file }; supported false for an AP without font files
//   await fonts.put(client, file)          PUT /api/fonts/{file.name}, file a parsed font file
//   await fonts.remove(client, name)       DELETE

// What a font file the user picked must look like before it goes to the AP; null, or why not
export function checkFontFile(file) {
  if (!file || typeof file !== "object" || file.format !== "esl-font")
    return 'not a font file (format "esl-font"); make one with tag/tools/gen_font.py --file';
  if (typeof file.name !== "string" || !/^[a-z0-9_-]{1,15}$/.test(file.name))
    return "its name must be 1 to 15 of a-z, 0-9, _ and -";
  return null;
}

export function apFonts() {
  const cache = new Map();   // "name:version" -> text

  async function read(client) {
    let info;
    try {
      info = await client.call("GET", "/api/fonts");
    } catch (e) {
      if (/^404\b/.test(e.message)) return { supported: false, info: null, files: {} };
      throw e;
    }
    const files = {};
    for (const f of info.fonts || []) {
      const key = f.name + ":" + f.version;
      if (!cache.has(key))
        cache.set(key, JSON.stringify(await client.call("GET", "/api/fonts/" + encodeURIComponent(f.name))));
      files[f.name] = cache.get(key);
    }
    return { supported: true, info, files };
  }

  return {
    read,
    put: (client, file) => client.call("PUT", "/api/fonts/" + encodeURIComponent(file.name), file),
    remove: (client, name) => client.call("DELETE", "/api/fonts/" + encodeURIComponent(name)),
  };
}
