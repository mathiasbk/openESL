// layoutapi.js - what a client sends for one layout: its fields in order, a form that sends
// them to a tag straight from the browser (PUT /api/tags/{mac}), and the same request as
// curl, JSON, esl-client and Home Assistant to copy. Shown in the dashboard's Layouts card
// (js/aplayouts.js) and in the designer's info window for a layout (js/editor.js).

// What a client sends for a layout, in the forms the panel offers: { curl, json, csv, ha }
export function examples({ layout, fields, sample, mac, host }) {
  const body = content(layout, sample.slice(0, fields.length));
  const json = JSON.stringify(body, null, 2);
  const target = mac || "<MAC>";
  const q = (s) => `"${String(s).replace(/"/g, '""')}"`;
  return {
    curl: `curl -X PUT http://${host}/api/tags/${target} \\\n  -H "Authorization: Bearer $TOKEN" \\\n` +
          `  -H "Content-Type: application/json" \\\n  -d '${JSON.stringify(body, null, 2).replace(/\n/g, "\n     ").replace(/'/g, "'\\''")}'`,
    json: `PUT /api/tags/${target}\n\n${json}`,
    csv: `# tags.csv\nmac,layout,${fields.join(",")}\n${target},${layout},${body.fields.map(q).join(",")}\n\n` +
         `python3 tools/esl-client/esl_client.py --url http://${host} --token $TOKEN sync tags.csv`,
    ha: `# rest_command from docs/home-assistant.md\naction: rest_command.esl_tag_text\ndata:\n` +
        `  mac: "${target}"\n  layout: ${layout}\n  fields: ${JSON.stringify(body.fields)}`,
  };
}

// The content to send: trailing empty fields are left out (they stay empty on the tag)
export function content(layout, values) {
  const f = values.map((s) => String(s ?? ""));
  while (f.length > 1 && !f[f.length - 1]) f.pop();
  return { layout, fields: f };
}

const TABS = [["curl", "curl"], ["json", "JSON"], ["csv", "esl-client"], ["ha", "Home Assistant"]];

function el(tag, cls, text) {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
}

const short = (mac) => "…" + mac.slice(-6);

// The panel, built into `root` (emptied). onClose: the ✕ in its head (none without it).
// show(opts) fills it for one layout:
//   key          what the typed values are kept under (model and layout name)
//   name, state  the layout's name and a pill ("On the AP", "Built in", "Not on the AP")
//   ok           whether the pill is green
//   desc         one line about it, or ""
//   fields       the field names in order; sample: their sample text; what: what each one is
//   tags         the tags it can go to, [{ mac, shows, shown }] (shows: it shows this layout
//                now; shown: the layout it shows, "" for none), or null when not known
//   host         the AP's address in the examples
//   send         async (mac, content) => reply, or null when it cannot be sent from here
//   why          with no send: what to do first, said under the form
//   first        with no send: { label, run } for the button, the step that makes it sendable
export function layoutPanel(root, { onClose } = {}) {
  const values = {};   // by key: the field values typed in, kept while the page is open
  let tab = "curl";
  let cur = null;
  let target = "";      // none until one is picked: a click must not overwrite a tag by itself
  let tagsKey = "";
  let busy = false;

  root.replaceChildren();
  const head = el("div", "lay-p-head");
  const h = el("h3");
  const name = el("span");
  const pill = el("span", "pill");
  h.append(name, " ", pill);
  head.append(h);
  if (onClose) {
    const x = el("button", "lay-x", "✕");
    x.type = "button";
    x.setAttribute("aria-label", "Close");
    x.onclick = onClose;
    head.append(x);
  }
  const desc = el("p", "muted");

  const sendH = el("h4", "lay-h", "Send to a tag");
  const form = el("form", "lay-send");
  const inputs = el("div", "lay-inputs");
  const row = el("div", "lay-send-row");
  const pick = el("select");
  pick.setAttribute("aria-label", "Tag");
  const go = el("button", "primary", "Send to tag");
  go.type = "submit";
  row.append(pick, go);
  const msg = el("p", "hint");
  msg.setAttribute("role", "status");
  form.append(inputs, row, msg);

  const apiH = el("h4", "lay-h", "Or from a program");
  const apiHint = el("p", "hint");
  apiHint.append("The same request, with the fields in this order as ", el("code", "", "fields"), ". Fields left out stay empty.");
  const tabsRow = el("div", "lay-p-tabs");
  const tabs = el("div", "seg small");
  tabs.setAttribute("role", "group");
  tabs.setAttribute("aria-label", "Example");
  const copy = el("button", "small", "Copy");
  copy.type = "button";
  tabsRow.append(tabs, copy);
  const code = el("pre", "lay-code");
  const usersH = el("h4", "lay-h");
  const users = el("p", "mono small");
  root.append(head, desc, sendH, form, apiH, apiHint, tabsRow, code, usersH, users);

  const typed = () => [...inputs.querySelectorAll("input")].map((i) => i.value);
  const say = (text, cls = "") => {
    msg.textContent = text;
    msg.className = "hint" + (cls ? " " + cls : "");
  };

  function drawCode() {
    if (!cur) return;
    const ex = examples({ layout: cur.name, fields: cur.fields, sample: typed(), mac: target, host: cur.host });
    code.textContent = ex[tab];
  }

  function drawTabs() {
    tabs.replaceChildren(...TABS.map(([k, label]) => {
      const b = el("button", k === tab ? "on" : "", label);
      b.type = "button";
      b.setAttribute("aria-pressed", String(k === tab));
      b.onclick = () => {
        tab = k;
        drawTabs();
        drawCode();
      };
      return b;
    }));
  }

  function canSend() {
    go.disabled = busy || (cur?.send ? !target : !cur?.first);
  }

  function show(opts) {
    const same = cur?.key === opts.key;
    cur = opts;
    name.textContent = opts.name;
    pill.textContent = opts.state;
    pill.className = "pill " + (opts.ok ? "ok" : "off");
    desc.textContent = opts.desc || "";
    desc.hidden = !opts.desc;

    // The inputs are made again only for another layout, so typing survives a refresh
    const was = typed();
    if (!same || was.length !== opts.fields.length) {
      const v = values[opts.key] || opts.sample.slice(0, opts.fields.length);
      inputs.replaceChildren(...opts.fields.map((f, i) => {
        const l = el("label");
        const n = el("span", "lay-f");
        n.append(el("code", "", f), el("span", "muted", " " + i));
        const inp = el("input");
        inp.value = v[i] ?? "";
        inp.placeholder = opts.what?.[i] || "";
        inp.title = opts.what?.[i] || "";
        inp.oninput = () => {
          values[opts.key] = typed();
          drawCode();
        };
        l.append(n, inp);
        return l;
      }));
      if (!same) say("");
    }

    // The tags of this model, those showing this layout first; the one picked stays picked.
    // The list is made again only when it changed, so an open list stays open at a refresh.
    const tags = [...(opts.tags || [])].sort((a, b) => Number(b.shows) - Number(a.shows));
    if (!tags.some((t) => t.mac === target)) target = "";
    const tk = JSON.stringify(tags.map((t) => [t.mac, t.shows]));
    if (tk !== tagsKey) {
      tagsKey = tk;
      const none = el("option", "", tags.length ? "Choose a tag…" : "No tags of this model");
      none.value = "";
      pick.replaceChildren(none, ...tags.map((t) => {
        const o = el("option", "", short(t.mac) + (t.shows ? " · shows it" : ""));
        o.value = t.mac;
        return o;
      }));
    }
    pick.value = target;
    pick.disabled = !tags.length;
    pick.hidden = !opts.send;
    go.textContent = opts.send ? "Send to tag" : opts.first?.label || "Send to tag";
    go.classList.toggle("primary", !!(opts.send || opts.first));
    if (!opts.send && opts.why) say(opts.why);
    canSend();

    drawTabs();
    drawCode();
    const u = tags.filter((t) => t.shows);
    usersH.textContent = opts.tags ? `Tags using it (${u.length})` : "Tags using it";
    users.textContent = !opts.tags ? "Connect to your AP to see them." : u.length ? u.map((t) => short(t.mac)).join(" · ") : "None yet.";
  }

  pick.onchange = () => {
    target = pick.value;
    canSend();
    drawCode();
  };
  form.onsubmit = async (ev) => {
    ev.preventDefault();
    if (!cur?.send) return cur?.first?.run();
    if (busy || !target) return;
    const c = content(cur.name, typed());
    const mac = target;
    const t = cur.tags?.find((x) => x.mac === mac);
    if (t?.shown && !confirm(`${short(mac)} ${t.shown === "?" ? "has content" : `shows "${t.shown}"`} now. Replace it with this?`)) return;
    busy = true;
    canSend();
    say(`Sending to ${short(mac)}…`);
    try {
      const r = await cur.send(mac, c);
      say(`Sent to ${short(mac)}. The tag shows it at its next check-in${r?.changed === false ? " (it had this already)" : ""}.`, "ok");
    } catch (e) {
      say(e.message, "error");
    } finally {
      busy = false;
      canSend();
    }
  };
  copy.onclick = async () => {
    try {
      await navigator.clipboard.writeText(code.textContent);
      copy.textContent = "Copied";
    } catch {
      copy.textContent = "Select and copy";
    }
    setTimeout(() => (copy.textContent = "Copy"), 1500);
  };

  return { show };
}
