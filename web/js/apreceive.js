// apreceive.js - on the AP's own page (/dashboard?from=designer): takes the layout file the
// designer on the web page hands over (js/handoff.js), shows it with the preview the designer
// drew and how it differs from the file this AP has, and stores it only when Save is pressed,
// with the dashboard's token. Then it can put the layout's sample text on a tag, as the
// designer's Send does, and tells the designer how it went.
import { ApClient } from "./apclient.js";
import { READY, SAVED, VERSION, QUERY, trustedOrigin, readLayoutsMessage, compareFiles } from "./handoff.js";

const $ = (id) => document.getElementById(id);

function names(list, cls) {
  return list.map((n) => Object.assign(document.createElement("span"), { className: "lname" + (cls ? " " + cls : ""), textContent: n }));
}

const kb = (n) => (n / 1024).toFixed(1).replace(/\.0$/, "") + " KB";

export function initReceive() {
  if (!new URLSearchParams(location.search).has(QUERY.split("=")[0])) return;
  // Drop ?from=designer, so a reload or a bookmark is the plain dashboard
  history.replaceState(null, "", location.pathname + location.hash);
  const opener = window.opener;
  let got = null;          // what the designer sent (readLayoutsMessage)
  let from = "";           // the designer's origin, for the answer
  let seq = 0;             // bumped by each describe() and save(): an older describe() stops
  let done = false;        // saved: describe() no longer redraws the card

  const card = $("recv");
  const say = (text, cls = "") => {
    $("recv-msg").textContent = text;
    $("recv-msg").className = "hint" + (cls ? " " + cls : "");
  };
  // The card's own field first: it is how a wrong token in the dashboard's field gets corrected
  const token = () => $("recv-token").value.trim() || $("dash-token").value.trim();
  const client = () => new ApClient("", token());
  const tell = (ok, text) => {
    if (opener && from) opener.postMessage({ type: SAVED, v: VERSION, model: got?.model, ok, text }, from);
  };

  // The AP's file and tags for the model, to compare with and to pick a tag from
  async function describe() {
    if (done) return;
    const n = ++seq;
    const { model, file } = got;
    if (!$("recv-token").value.trim()) $("recv-token-label").hidden = !!$("dash-token").value.trim();
    let current = null, list = null, max = 0, err = "";
    if (token()) {
      try {
        current = await client().call("GET", "/api/layouts/" + encodeURIComponent(model));
      } catch (e) {
        if (/^401\b/.test(e.message)) $("recv-token-label").hidden = false;
        if (!/^404\b/.test(e.message)) err = "Could not read the AP's layout file: " + e.message;
      }
      try { max = (await client().layouts()).max_size || 0; } catch { /* older AP: no limit shown */ }
      try { list = (await client().tags()).tags || []; } catch { /* shown without tags */ }
    }
    // Save was pressed (or the token changed again) while this read: leave the card to that
    if (n !== seq || done) return;
    const d = compareFiles(file, current);
    const what = $("recv-what");
    what.replaceChildren(`For ${model}" tags: `, ...names(d.kept), ...names(d.added, current ? "new" : ""), ...names(d.removed, "gone"));
    const bytes = new TextEncoder().encode(JSON.stringify(file)).length;
    const notes = [];
    notes.push(current ? `It replaces the ${model}" layout file on this AP.` : token() ? `This AP has no ${model}" layout file yet.` : "");
    if (d.removed.length) notes.push(`Not in this file, so gone from the AP after saving: ${d.removed.join(", ")}. Tags that use ${d.removed.length === 1 ? "it" : "them"} keep their picture until they get other content.`);
    notes.push(`Tags that use these layouts get their new look at their next check-in. Size ${kb(bytes)}${max ? " of " + kb(max) : ""}.`);
    $("recv-note").textContent = notes.filter(Boolean).join(" ");
    // The tags of this model, for the sample text
    const sel = $("recv-tag");
    const mine = (list || []).filter((t) => t.model === model);
    sel.replaceChildren(new Option("no tag", ""), ...mine.map((t) => new Option(`…${t.mac.slice(-6)}`, t.mac)));
    $("recv-tag-label").hidden = !got.content || !mine.length;
    say(err || (token() ? "" : "Enter the AP's API token to save it here."), err ? "error" : "");
  }

  async function save() {
    if (!token()) {
      $("recv-token-label").hidden = false;
      return say("Enter the AP's API token first.", "error");
    }
    $("recv-save").disabled = true;
    seq++;
    say("Saving …");
    try {
      const r = await client().putLayouts(got.model, got.file);
      done = true;
      let text = r.changed ? `Saved on the AP. The ${got.model}" tags that use it get their new look at their next check-in.` : "The AP already had this file.";
      let errs = r.content_errors_total || 0;
      if (errs) text += ` ${errs === 1 ? "1 tag has" : errs + " tags have"} content that does not fit it and keep${errs === 1 ? "s" : ""} what ${errs === 1 ? "it shows" : "they show"}: see the table below.`;
      const mac = $("recv-tag").value;
      if (mac && got.content) {
        // The file is saved whatever this answers
        try {
          await client().putContent(mac, got.content);
          text += ` Tag …${mac.slice(-6)} gets "${got.content.layout}" with the sample text.`;
        } catch (e) {
          text += ` Tag …${mac.slice(-6)} did not get the sample text: ${e.message}`;
          errs++;
        }
      }
      // The token worked: keep it for the dashboard, as its own field does
      if (token() !== $("dash-token").value.trim()) {
        $("dash-token").value = token();
        try { localStorage.setItem("openesl.dash.token", token()); } catch { /* private window */ }
        // Connected with the old token: disconnect first (the button toggles), then connect
        if ($("dash-connect").textContent === "Disconnect") $("dash-connect").click();
        $("dash-connect").click();
      }
      say(text, errs ? "error" : "ok");
      $("recv-buttons").hidden = $("recv-tag-label").hidden = $("recv-token-label").hidden = $("recv-note").hidden = true;
      tell(true, text);
    } catch (e) {
      if (/^401\b/.test(e.message)) {
        $("recv-token-label").hidden = false;
        say("Not saved: the AP did not accept the token. Enter the right one above and press Save again.", "error");
      } else say("Not saved: " + e.message, "error");
      tell(false, "The AP did not save it: " + e.message);
    } finally {
      $("recv-save").disabled = false;
    }
  }

  window.addEventListener("message", (ev) => {
    if (ev.source !== opener || !trustedOrigin(ev.origin)) return;
    const m = readLayoutsMessage(ev.data);
    card.hidden = false;
    if (m.error) {
      $("recv-buttons").hidden = true;
      return say("The designer's message could not be read: " + m.error + ".", "error");
    }
    got = m;
    from = ev.origin;
    $("recv-img").hidden = !m.preview;
    if (m.preview) $("recv-img").src = m.preview;
    // A built-in layout (price, text) is not in the file: then it is the file that came
    const inFile = m.layout && (m.file.layouts || []).some((l) => l.name === m.layout);
    $("recv-h").textContent = inFile ? `The designer sent you "${m.layout}"` : `The designer sent you the ${m.model}" layout file`;
    $("recv-buttons").hidden = $("recv-note").hidden = false;
    say("");
    describe();
  });
  $("recv-save").addEventListener("click", save);
  $("recv-drop").addEventListener("click", () => {
    card.hidden = true;
    tell(false, "Thrown away on the AP's page; nothing was saved.");
  });
  $("recv-token").addEventListener("change", () => got && describe());
  $("dash-token").addEventListener("change", () => got && describe());

  if (!opener) {
    card.hidden = false;
    $("recv-buttons").hidden = true;
    say("Nothing came from the designer: open this page with Open my AP in the designer's Send dialog.", "error");
    return;
  }
  // The designer waits for this before it sends; its origin is not known yet, and this says nothing secret
  opener.postMessage({ type: READY, v: VERSION }, "*");
}
