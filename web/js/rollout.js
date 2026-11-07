// rollout.js - new tag firmware to one tag of a model first (#306, docs/release.md "Rollout to
// the tags"): how far the tags that run a version have got with it, and whether it is safe to
// give it to the rest. No DOM here, so test/rollout.test.mjs can check it.
//
// The AP counts a tag's polls only since it started (signal.polls in GET /api/tags) and does not
// say since when the tag runs its firmware, so the page keeps that itself, per tag, in this
// browser (trackRollout): the poll count when it first saw the tag on a version, carried over
// AP restarts. It counts from the first time this page sees the tag on the version, so it can
// only count too few polls, never too many.

import { modelName } from "./models.js";

export const ROLLOUT_POLLS = 10; // polls on the new firmware before the rest of the model gets it
export const ROLLOUT_PROBLEMS = ["silent", "draw_failed", "content_error"]; // GET /api/health problems that count against it
// Problems count against a version while a tag makes its first polls on it (the trial). Later
// ones (a battery running out weeks on) are not the firmware's.
export const TRIAL_POLLS = 3 * ROLLOUT_POLLS;
const BOOT_SLACK_S = 120; // uptime_s and the clock are read at slightly different moments
const MAX_ENTRIES = 3000;

// "0.11.0" -> 1100, as the tag's fw (major*10000 + minor*100 + patch); null for anything else
export function verNum(v) {
  const m = /^(\d+)\.(\d+)\.(\d+)$/.exec(v || "");
  return m ? Number(m[1]) * 10000 + Number(m[2]) * 100 + Number(m[3]) : null;
}

// The state after one more read of the AP: prev (what was kept, { mac: entry }), list (GET
// /api/tags), health (GET /api/health), nowS (Unix seconds). An entry is { fw, boot, base, last,
// carried, bad }: the tag's version, when the AP started (estimated from uptime_s), its poll
// count at the first sight on fw, at the last sight, the polls from before an AP restart, and the
// problems seen on fw in its first TRIAL_POLLS polls; with next: { fw, bad } the problems seen
// while it installs other firmware (all of it fetched, fw_update in GET /api/tags): a tag that
// installs new firmware and goes silent still runs its old fw for the AP, and that is the new
// firmware's problem. A tag the AP has not heard since it started (no signal) keeps its entry.
export function trackRollout(prev, list, health, nowS) {
  const state = { ...(prev || {}) };
  const uptime = list?.ap?.uptime_s;
  const boot = Number.isFinite(uptime) ? Math.round(nowS - uptime) : null;
  const probs = new Map((health?.problems || []).map((p) => [p.mac, p.problems || []]));
  const here = new Set();
  for (const tag of list?.tags || []) {
    here.add(tag.mac);
    const polls = tag.signal?.polls;
    if (tag.fw == null || !Number.isInteger(polls)) continue;
    let e = state[tag.mac];
    if (!e || e.fw !== tag.fw) {
      // what it had while installing this firmware stays with it
      const had = e?.next?.fw === tag.fw ? [...e.next.bad] : [];
      e = { fw: tag.fw, boot, base: polls, last: polls, carried: 0, bad: had };
    } else {
      e = { ...e, bad: [...e.bad] };
      // The AP restarted: its count began again at 0, every poll since then is on fw
      const restarted = polls < e.last || (boot != null && e.boot != null && Math.abs(boot - e.boot) > BOOT_SLACK_S);
      if (restarted) {
        e.carried += Math.max(0, e.last - e.base);
        e.base = 0;
        e.boot = boot;
      }
      e.last = polls;
    }
    const found = (probs.get(tag.mac) || []).filter((p) => ROLLOUT_PROBLEMS.includes(p));
    const u = tag.fw_update;
    const target = u && u.size && u.received >= u.size ? verNum(u.version) : null;
    if (target != null && target !== tag.fw) {
      // installing: its problems are the new firmware's
      const next = e.next?.fw === target ? { fw: target, bad: [...e.next.bad] } : { fw: target, bad: [] };
      for (const p of found) if (!next.bad.includes(p)) next.bad.push(p);
      e.next = next;
    } else {
      if (!u) delete e.next;
      // still fetching other firmware: it runs fw, so its problems are fw's
      if (pollsOn(e, tag) < TRIAL_POLLS) for (const p of found) if (!e.bad.includes(p)) e.bad.push(p);
    }
    state[tag.mac] = e;
  }
  // Tags of other APs stay, unless there are very many
  const macs = Object.keys(state);
  if (macs.length > MAX_ENTRIES) for (const m of macs) if (!here.has(m)) delete state[m];
  return state;
}

// The polls this page has seen the tag make on the firmware it runs (null: not seen on it yet)
export function pollsOn(entry, tag) {
  if (!entry || tag?.fw == null || entry.fw !== tag.fw) return null;
  return entry.carried + Math.max(0, entry.last - entry.base);
}

// How far a version is tried on the tags of a model: { ver, tried, polls, ready, bad, failed,
// proven, text }. tried: the tags that run it; polls: the most polls one of those has made
// without a problem; ready: how many have made ROLLOUT_POLLS without one; bad: [{ mac,
// problems }] the tags with a problem on it; failed: the tags that rejected or rolled back the
// AP's image (tags.failed in GET /api/tag-firmware). proven: at least one tag ran it for
// ROLLOUT_POLLS polls, and no tag had a problem with it or rejected it.
export function modelTrial(model, version, list, state, failed = 0) {
  const ver = typeof version === "number" ? version : verNum(version);
  const on = (list?.tags || []).filter((t) => t.model === model && ver != null && t.fw === ver);
  let polls = 0, ready = 0;
  const bad = [];
  // tags that went quiet (or worse) while installing it: still on their old fw for the AP
  for (const t of list?.tags || []) {
    const n = state?.[t.mac]?.next;
    if (t.model === model && t.fw !== ver && n?.fw === ver && n.bad.length) bad.push({ mac: t.mac, problems: n.bad.map((p) => p + "_installing") });
  }
  for (const t of on) {
    const e = state?.[t.mac];
    const n = pollsOn(e, t) ?? 0;
    if (e?.fw === t.fw && e.bad.length) {
      bad.push({ mac: t.mac, problems: e.bad });
      continue;
    }
    polls = Math.max(polls, n);
    if (n >= ROLLOUT_POLLS) ready++;
  }
  const proven = ready > 0 && !bad.length && !failed;
  return { ver, tried: on.length, polls, ready, bad, failed, proven, text: trialText({ tried: on.length, polls, ready, bad, failed, proven }) };
}

const PROBLEM_WORDS = {
  silent: "silent", draw_failed: "drawing failed", content_error: "content cannot be shown",
  silent_installing: "silent since it installed it", draw_failed_installing: "drawing failed while installing it",
  content_error_installing: "content cannot be shown while installing it",
};

function trialText({ tried, polls, ready, bad, failed, proven }) {
  const tags = (n) => (n === 1 ? "1 tag" : n + " tags");
  if (!tried && !failed && !bad.length) return "not tried on a tag yet";
  const parts = [];
  if (tried) parts.push(`runs on ${tags(tried)}`);
  if (failed) parts.push(`${tags(failed)} rejected it or went back`);
  for (const b of bad.slice(0, 2)) parts.push(`${b.mac}: ${b.problems.map((p) => PROBLEM_WORDS[p] || p).join(", ")}`);
  if (bad.length > 2) parts.push(`${bad.length - 2} more with problems`);
  if (proven) parts.push(`${ready === 1 ? "1 tag has" : ready + " tags have"} polled ${ROLLOUT_POLLS}+ times without problems: safe for the rest`);
  else if (tried > bad.length && !ready) parts.push(`${Math.min(polls, ROLLOUT_POLLS)} of ${ROLLOUT_POLLS} polls without problems so far`);
  return parts.join("; ");
}

// The tag to try new firmware on first: of the model's tags that take firmware over the radio
// and run older firmware than ver, one that is not late and has no problems, with the best signal.
// rows: tagRows() (or { tag, problems, next }); among: MACs to pick from (null: all).
export function firstTag(rows, model, ver, among = null) {
  // older firmware only: a tag on a newer (test) build would be a downgrade
  const ok = (rows || []).filter(({ tag }) => tag.model === model && tag.ota && tag.fw != null && tag.fw < ver && !tag.fw_update && (!among || among.includes(tag.mac)));
  const score = ({ tag, problems, next }) =>
    (problems?.length ? 2000 : 0) + (next?.late ? 1000 : 0) + (tag.seen_s_ago == null ? 500 : 0) - (Number.isFinite(tag.rssi) ? tag.rssi : -127);
  return ok.sort((a, b) => score(a) - score(b) || a.tag.mac.localeCompare(b.tag.mac))[0]?.tag || null;
}

// Whether the AP can give an image to chosen tags: every image it has says who it is for ("to").
// AP firmware from before that (#88) gives an uploaded image to every tag of its model.
export function apCanChoose(tagfw) {
  return !(tagfw?.images || []).some((i) => i.version && !("to" in i));
}

// What to ask before firmware goes to several tags of a model that no tag has run long enough,
// or to any more tags when tags have had problems with it (null: nothing to ask). count: how
// many tags get it with this send.
export function rolloutQuestion(model, version, count, trial) {
  const trouble = trial.bad.length > 0 || trial.failed > 0;
  if (count < 1 || (count === 1 && !trouble) || trial.proven) return null;
  const v = typeof version === "number" ? `${Math.floor(version / 10000)}.${Math.floor(version / 100) % 100}.${version % 100}` : version;
  const why = trouble
    ? `${modelName(model)} tags that got ${v} have had problems with it (${trial.text}).`
    : trial.tried
      ? `No ${modelName(model)} tag has polled ${ROLLOUT_POLLS} times on ${v} yet (${trial.text}).`
      : `No ${modelName(model)} tag runs ${v} yet.`;
  return `${count} ${modelName(model)} ${count === 1 ? "tag" : "tags"} would get firmware ${v}. ${why} A tag whose new firmware does not start has to go back in the jig, so try it on one tag first and give it to the rest once that tag has polled ${ROLLOUT_POLLS} times without problems.`;
}
