/** Pure profile/history state for Rig Lab. No DOM, storage or network access. */

const TIME_EPSILON = 1e-7;
const DRAFT_FORMAT = 'openminerz-rig-lab-draft';
const DRAFT_VERSION = 1;

function record(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value)
    && (Object.getPrototypeOf(value) === Object.prototype || Object.getPrototypeOf(value) === null);
}

function jsonValue(value) {
  if (value === null || typeof value === 'string' || typeof value === 'boolean') return;
  if (typeof value === 'number' && Number.isFinite(value)) return;
  if (Array.isArray(value) || record(value)) {
    for (const child of Object.values(value)) jsonValue(child);
    return;
  }
  throw new TypeError('Profiles must contain finite JSON values');
}

/** Copy old profiles without models into the current, otherwise unchanged schema. */
export function cloneProfile(profile) {
  if (!record(profile) || profile.schema !== 1) throw new TypeError('Expected a schema 1 rig profile');
  jsonValue(profile);
  const result = structuredClone(profile);
  result.clips ??= {};
  result.models ??= {};
  if (!record(result.clips) || !record(result.models)) throw new TypeError('Profile clips and models must be objects');
  return result;
}

function freeze(value) {
  if (value && typeof value === 'object') {
    for (const child of Object.values(value)) freeze(child);
    Object.freeze(value);
  }
  return value;
}

function canonical(value) {
  if (Array.isArray(value)) return '[' + value.map(canonical).join(',') + ']';
  if (record(value)) return '{' + Object.keys(value).sort().map(
    key => JSON.stringify(key) + ':' + canonical(value[key])).join(',') + '}';
  return JSON.stringify(value);
}

function snapshot(profile) {
  const state = freeze(cloneProfile(profile));
  return {state, fingerprint: canonical(state)};
}

function reduce(state, action) {
  if (typeof action !== 'function') return snapshot(action);
  const draft = cloneProfile(state);
  const result = action(draft);
  return snapshot(result === undefined ? draft : result);
}

/**
 * begin(label), repeated update(action), commit() form one undoable gesture.
 * Actions receive a mutable copy and may mutate it or return a replacement.
 * All exposed states are frozen snapshots; keep UI selection outside History.
 */
export class History {
  #present;
  #saved;
  #revision;
  #past = [];
  #future = [];
  #gesture = null;
  #limit;

  constructor(initial, {saved = initial, revision = null, limit = 100} = {}) {
    if (!Number.isInteger(limit) || limit < 1) throw new RangeError('History limit must be a positive integer');
    this.#limit = Math.min(100, limit);
    this.#present = snapshot(initial);
    this.#saved = snapshot(saved);
    this.#revision = revision;
  }

  get state() { return this.#present.state; }
  get savedState() { return this.#saved.state; }
  get savedRevision() { return this.#revision; }
  get dirty() { return this.#present.fingerprint !== this.#saved.fingerprint; }
  get transactionActive() { return this.#gesture !== null; }
  get canUndo() { return this.#past.length > 0 || (this.#gesture !== null && this.#gesture.before.fingerprint !== this.#present.fingerprint); }
  get canRedo() { return this.#future.length > 0 && !(this.#gesture !== null && this.#gesture.before.fingerprint !== this.#present.fingerprint); }
  get undoLabel() { return this.#gesture?.label ?? this.#past.at(-1)?.label ?? ''; }
  get redoLabel() { return this.#future.at(-1)?.label ?? ''; }

  begin(label = 'Edit profile') {
    if (this.#gesture !== null) throw new Error('Finish the current gesture before beginning another');
    this.#gesture = {before: this.#present, label};
  }

  update(action) {
    if (this.#gesture === null) return this.apply(action);
    this.#present = reduce(this.state, action);
    return this.state;
  }

  commit() {
    if (this.#gesture === null) return false;
    const {before, label} = this.#gesture;
    this.#gesture = null;
    if (before.fingerprint === this.#present.fingerprint) return false;
    this.#past.push({snapshot: before, label});
    if (this.#past.length > this.#limit) this.#past.splice(0, this.#past.length - this.#limit);
    this.#future = [];
    return true;
  }

  cancel() {
    if (this.#gesture === null) return false;
    this.#present = this.#gesture.before;
    this.#gesture = null;
    return true;
  }

  apply(action, label = 'Edit profile') {
    if (this.#gesture !== null) throw new Error('Commit or cancel the current gesture before applying an action');
    // Evaluate before opening the transaction: a rejected input leaves no
    // half-open gesture or lost redo branch.
    const next = reduce(this.state, action);
    this.begin(label);
    this.#present = next;
    this.commit();
    return this.state;
  }

  replace(profile, label = 'Replace profile') { return this.apply(profile, label); }

  undo() {
    this.commit();
    const entry = this.#past.pop();
    if (!entry) return false;
    this.#future.push({snapshot: this.#present, label: entry.label});
    this.#present = entry.snapshot;
    return true;
  }

  redo() {
    this.commit();
    const entry = this.#future.pop();
    if (!entry) return false;
    this.#past.push({snapshot: this.#present, label: entry.label});
    this.#present = entry.snapshot;
    return true;
  }

  /** Pass the profile actually sent to the server, not a later edited state. */
  markSaved(savedProfile = this.state, revision = this.#revision) {
    this.#saved = snapshot(savedProfile);
    this.#revision = revision;
  }
}

function name(value, kind) {
  if (typeof value !== 'string' || value.length === 0) throw new TypeError(`Expected a ${kind} name`);
  return value;
}

function time(value) {
  if (!Number.isFinite(value) || value < 0) throw new TypeError('Key time must be finite and nonnegative');
  return value;
}

function trackName(track) {
  if (track !== 'left' && track !== 'gun') throw new TypeError('Explicit keys use the left or gun track');
  return track;
}

function setOwn(object, key, value) {
  Object.defineProperty(object, key, {value, writable: true, configurable: true, enumerable: true});
}

/** Move an explicit key by removing its old time AND replacing its destination. */
export function upsertContactKey(profile, clip, oldTime, key, track = 'left') {
  name(clip, 'clip');
  trackName(track);
  if (!record(key)) throw new TypeError('Expected a contact key');
  time(key.time);
  if (oldTime !== null && oldTime !== undefined) time(oldTime);
  jsonValue(key);
  const next = cloneProfile(profile);
  const edits = Object.hasOwn(next.clips, clip) ? next.clips[clip] : {};
  const keys = (edits[track] ?? []).filter(existing =>
    Math.abs(existing.time - key.time) > TIME_EPSILON &&
    (oldTime === null || oldTime === undefined || Math.abs(existing.time - oldTime) > TIME_EPSILON));
  keys.push(structuredClone(key));
  keys.sort((a, b) => a.time - b.time);
  edits[track] = keys;
  setOwn(next.clips, clip, edits);
  return next;
}

/** Delete only a user override; the generated contact remains available. */
export function deleteContactKey(profile, clip, at, track = 'left') {
  name(clip, 'clip');
  trackName(track);
  time(at);
  const next = cloneProfile(profile);
  if (!Object.hasOwn(next.clips, clip)) return next;
  const edits = next.clips[clip];
  const keys = (edits[track] ?? []).filter(key => Math.abs(key.time - at) > TIME_EPSILON);
  if (keys.length) edits[track] = keys;
  else delete edits[track];
  if (Object.keys(edits).length === 0) delete next.clips[clip];
  return next;
}

function restoreEntry(target, defaults, key) {
  if (Object.hasOwn(defaults, key)) setOwn(target, key, structuredClone(defaults[key]));
  else delete target[key];
}

/** Clip reset leaves linked model edits intact. Wrap in history.apply to undo. */
export function resetClip(profile, defaultProfile, clip) {
  name(clip, 'clip');
  const next = cloneProfile(profile);
  const defaults = cloneProfile(defaultProfile);
  restoreEntry(next.clips, defaults.clips, clip);
  return next;
}

/** Exact catalog membership, never a name/family prefix, defines model scope. */
export function resetModel(profile, defaultProfile, catalog, model) {
  name(model, 'model');
  if (!record(catalog) || !Array.isArray(catalog.clips)) throw new TypeError('Expected a clip catalog');
  const next = cloneProfile(profile);
  const defaults = cloneProfile(defaultProfile);
  restoreEntry(next.models, defaults.models, model);
  for (const clip of catalog.clips) {
    if (clip.model === model) restoreEntry(next.clips, defaults.clips, name(clip.name, 'clip'));
  }
  return next;
}

export function resetAll(profile, defaultProfile) {
  // Accept the same reducer signature as scoped resets, without mutating or
  // retaining either argument. Defaults also restore rig/finger calibration.
  return cloneProfile(defaultProfile);
}

/** Serialize only a profile and its saved baseline revision, never UI state. */
export function serializeDraft(profile, savedRevision, {updatedAt = Date.now()} = {}) {
  if (savedRevision !== null && (typeof savedRevision !== 'string' || savedRevision.length === 0)) {
    throw new TypeError('Saved revision must be a nonempty string or null');
  }
  if (!Number.isFinite(updatedAt) || updatedAt < 0) throw new TypeError('Draft timestamp must be finite and nonnegative');
  return JSON.stringify({format: DRAFT_FORMAT, version: DRAFT_VERSION, savedRevision,
    updatedAt, profile: cloneProfile(profile)});
}

/**
 * Returns {status:'none'|'invalid'|'stale'|'recoverable', ...}.
 * Only recoverable may be offered as a matching-baseline recovery. Stale
 * content remains available for explicit review, but is never applied here.
 */
export function recoverDraft(raw, currentRevision) {
  if (raw === null || raw === undefined || raw === '') return {status: 'none'};
  try {
    const draft = JSON.parse(raw);
    if (!record(draft) || draft.format !== DRAFT_FORMAT || draft.version !== DRAFT_VERSION
        || !Number.isFinite(draft.updatedAt) || draft.updatedAt < 0
        || (draft.savedRevision !== null && (typeof draft.savedRevision !== 'string' || !draft.savedRevision))) {
      throw new TypeError('Unrecognized local draft');
    }
    const profile = freeze(cloneProfile(draft.profile));
    const verified = typeof currentRevision === 'string' && currentRevision.length > 0
      && typeof draft.savedRevision === 'string' && draft.savedRevision === currentRevision;
    return {status: verified ? 'recoverable' : 'stale', profile,
      savedRevision: draft.savedRevision, updatedAt: draft.updatedAt,
      ...(verified ? {} : {reason: 'The local draft does not match the current saved revision'})};
  } catch (error) {
    return {status: 'invalid', reason: error.message};
  }
}
