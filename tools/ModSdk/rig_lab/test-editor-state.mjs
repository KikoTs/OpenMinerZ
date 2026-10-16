import assert from 'node:assert/strict';
import test from 'node:test';
import {
  History, cloneProfile, upsertContactKey, deleteContactKey,
  resetClip, resetModel, resetAll, serializeDraft, recoverDraft,
} from './editor_state.js';

function profile() {
  return {schema: 1, bones: {WristLeft: 'WristLeft'}, palms: {Left: [.096, -.055, .004]},
    finger_curl: {Index: [55, 78, 40]}, clips: {}, models: {}};
}

function contact(time, x = 0) {
  return {time, space: 'gun', position: [x, .1, .05], rotation: [0, 0, 0, 1], easing: 'smooth'};
}

test('history owns frozen copies of input, action result and saved baseline', () => {
  const initial = profile();
  const history = new History(initial, {revision: 'saved-1'});
  initial.palms.Left[0] = 99;
  assert.equal(history.state.palms.Left[0], .096);
  assert.throws(() => { history.state.palms.Left[0] = 99; }, TypeError);
  const replacement = profile();
  replacement.clips.PistolReload = {left: [contact(.5)]};
  history.replace(replacement);
  replacement.clips.PistolReload.left[0].position[0] = 99;
  assert.equal(history.state.clips.PistolReload.left[0].position[0], 0);
  assert.equal(history.savedState.clips.PistolReload, undefined);
});

test('one drag with many updates is one undo step; cancel preserves redo', () => {
  const history = new History(profile());
  history.begin('Move contact');
  for (let i = 0; i < 12; i++) history.update(state => upsertContactKey(state, 'PistolReload', .4, contact(.4, i)));
  assert.equal(history.dirty, true);
  assert.equal(history.commit(), true);
  assert.equal(history.undoLabel, 'Move contact');
  assert.equal(history.undo(), true);
  assert.equal(history.dirty, false);
  assert.equal(history.canUndo, false);
  history.begin('Cancelled edit');
  history.update(state => { state.palms.Left[0] = .12; });
  history.cancel();
  assert.equal(history.canRedo, true);
  history.redo();
  assert.equal(history.state.clips.PistolReload.left[0].position[0], 11);
});

test('retiming removes the old override and replaces an occupied destination', () => {
  const initial = profile();
  initial.clips.PistolReload = {left: [contact(.2, 2), contact(.5, 5), contact(.8, 8)],
    gun: [{time: .2, value: [0, 0, 0, 0, 0, 0]}]};
  const next = upsertContactKey(initial, 'PistolReload', .2, contact(.5, 9));
  assert.deepEqual(next.clips.PistolReload.left.map(key => [key.time, key.position[0]]), [[.5, 9], [.8, 8]]);
  assert.equal(initial.clips.PistolReload.left.length, 3);
  assert.deepEqual(next.clips.PistolReload.gun, initial.clips.PistolReload.gun);
  const inserted = upsertContactKey(next, 'PistolReload', null, contact(.1));
  assert.deepEqual(inserted.clips.PistolReload.left.map(key => key.time), [.1, .5, .8]);
  const movedGun = upsertContactKey(next, 'PistolReload', .2,
    {time: .4, value: [1, 0, 0, 0, 0, 0]}, 'gun');
  assert.deepEqual(movedGun.clips.PistolReload.gun.map(key => key.time), [.4]);
});

test('key deletion removes only the explicit override and preserves other tracks', () => {
  let initial = upsertContactKey(profile(), 'PistolReload', null, contact(.4));
  initial.clips.PistolReload.gun = [{time: 0, value: [0, 0, 0, 0, 0, 0]}];
  const withoutLeft = deleteContactKey(initial, 'PistolReload', .4 + 1e-9);
  assert.equal(withoutLeft.clips.PistolReload.left, undefined);
  assert.equal(withoutLeft.clips.PistolReload.gun.length, 1);
  const withoutClip = deleteContactKey(withoutLeft, 'PistolReload', 0, 'gun');
  assert.deepEqual(withoutClip.clips, {});
  assert.equal(initial.clips.PistolReload.left.length, 1);
});

test('a retime gesture advances the removed key while preserving its authored source', () => {
  for (const track of ['left', 'gun']) {
    const initial = profile();
    const key = track === 'left' ? contact(.2) : {time: .2, value: [0, 0, 0, 0, 0, 0]};
    initial.clips.PistolReload = {[track]: [key]};
    const history = new History(initial);
    let previousTime = key.time;
    history.begin('Retime key');
    for (const newTime of [.3, .4, .6]) {
      const moved = {...key, sourceTime: key.time, time: newTime};
      history.update(state => upsertContactKey(state, 'PistolReload', previousTime, moved, track));
      previousTime = newTime;
    }
    history.commit();
    assert.deepEqual(history.state.clips.PistolReload[track].map(k => [k.time, k.sourceTime]), [[.6, .2]]);
    history.undo();
    assert.deepEqual(history.state, initial);
    assert.equal(history.canUndo, false);
  }
});

test('history spans clip changes and can undo a clip reset without resetting its model', () => {
  const defaults = Object.freeze(profile());
  const history = new History(profile());
  history.apply(state => upsertContactKey(state, 'PistolReload', null, contact(.4)), 'Pistol contact');
  history.apply(state => upsertContactKey(state, 'SMGReload', null, contact(.8)), 'SMG contact');
  history.apply(state => { state.models.pistol = {held: {position: [1, 2, 3]}}; }, 'Linked grip');
  history.apply(state => resetClip(state, defaults, 'PistolReload'), 'Reset pistol clip');
  assert.equal(history.state.clips.PistolReload, undefined);
  assert.equal(history.state.models.pistol.held.position[0], 1);
  assert.equal(history.state.clips.SMGReload.left.length, 1);
  history.undo();
  assert.equal(history.state.clips.PistolReload.left.length, 1);
  history.undo();
  assert.equal(history.state.models.pistol, undefined);
  history.undo();
  assert.equal(history.state.clips.SMGReload, undefined);
  assert.equal(history.state.clips.PistolReload.left.length, 1);
  history.redo();
  assert.equal(history.state.clips.SMGReload.left.length, 1);
  assert.deepEqual(defaults.clips, {});
});

test('model reset uses exact catalog model membership, including distinct classic variants', () => {
  const defaults = profile();
  const initial = profile();
  for (const clip of ['AKIdle', 'AKReload', 'ClassicAKReload', 'AKLikeName', 'NotInCatalog']) {
    initial.clips[clip] = {left: [contact(.3)]};
  }
  initial.models = {ak: {held: 1}, 'ak-classic': {held: 2}, pistol: {held: 3}};
  const catalog = {clips: [
    {name: 'AKIdle', model: 'ak'}, {name: 'AKReload', model: 'ak'},
    {name: 'ClassicAKReload', model: 'ak-classic'}, {name: 'AKLikeName', model: 'pistol'},
  ]};
  const reset = resetModel(initial, defaults, catalog, 'ak');
  assert.deepEqual(Object.keys(reset.clips).sort(), ['AKLikeName', 'ClassicAKReload', 'NotInCatalog']);
  assert.deepEqual(reset.models, {'ak-classic': {held: 2}, pistol: {held: 3}});
  assert.equal(initial.clips.AKIdle.left.length, 1);
  assert.equal(initial.models.ak.held, 1);
});

test('reset all restores default calibration and empty maps as an undoable action', () => {
  const defaults = profile();
  const initial = profile();
  initial.bones.WristLeft = 'ElbowLeft';
  initial.clips.PistolReload = {left: [contact(.2)]};
  initial.models.pistol = {held: {position: [1, 2, 3]}};
  const history = new History(initial);
  history.apply(state => resetAll(state, defaults), 'Reset all');
  assert.deepEqual(history.state, defaults);
  history.undo();
  assert.deepEqual(history.state, initial);
  assert.deepEqual(defaults.clips, {});
  assert.deepEqual(defaults.models, {});
  const oldProfile = profile();
  delete oldProfile.models;
  assert.deepEqual(cloneProfile(oldProfile).models, {});
  assert.equal(Object.hasOwn(oldProfile, 'models'), false);
});

test('saved baseline tracks content across undo/redo and asynchronous save completion', () => {
  const history = new History(profile(), {revision: 'r1'});
  history.apply(state => upsertContactKey(state, 'PistolReload', null, contact(.4)));
  const sentToServer = history.state;
  history.apply(state => upsertContactKey(state, 'SMGReload', null, contact(.6)));
  history.markSaved(sentToServer, 'r2');
  assert.equal(history.savedRevision, 'r2');
  assert.equal(history.dirty, true);
  history.undo();
  assert.equal(history.dirty, false);
  history.undo();
  assert.equal(history.dirty, true);
  history.redo();
  assert.equal(history.dirty, false);
  history.markSaved();
  assert.equal(history.dirty, false);
});

test('semantic no-op gestures do not mark dirty or discard redo', () => {
  const history = new History(profile());
  history.apply(state => { state.palms.Left[0] = .1; });
  history.undo();
  history.begin();
  history.update(state => { state.palms.Left[0] = .2; });
  history.update(state => { state.palms.Left[0] = .096; });
  assert.equal(history.commit(), false);
  assert.equal(history.dirty, false);
  assert.equal(history.canRedo, true);
  history.apply(state => ({...state, bones: {...state.bones}}));
  assert.equal(history.canRedo, true);
});

test('a committed edit after undo invalidates redo; a rejected edit does not', () => {
  const history = new History(profile());
  history.apply(state => { state.palms.Left[0] = .1; });
  history.undo();
  assert.throws(() => history.apply(state => { state.palms.Left[0] = NaN; }), TypeError);
  assert.equal(history.transactionActive, false);
  assert.equal(history.canRedo, true);
  history.apply(state => { state.palms.Left[0] = .2; });
  assert.equal(history.canRedo, false);
  assert.equal(history.redo(), false);
});

test('history retains at most 100 committed gestures', () => {
  const history = new History(profile(), {limit: 1000});
  for (let i = 1; i <= 105; i++) history.apply(state => { state.palms.Left[0] = i; });
  let count = 0;
  while (history.undo()) count++;
  assert.equal(count, 100);
  assert.equal(history.state.palms.Left[0], 5);
  for (let i = 0; i < 100; i++) assert.equal(history.redo(), true);
  assert.equal(history.state.palms.Left[0], 105);
});

test('draft recovery requires the exact saved revision and never replaces live state', () => {
  const draftProfile = upsertContactKey(profile(), 'PistolReload', null, contact(.4));
  const serialized = serializeDraft(draftProfile, 'revision-a', {updatedAt: 1234});
  draftProfile.clips.PistolReload.left[0].position[0] = 99;
  const recovered = recoverDraft(serialized, 'revision-a');
  assert.equal(recovered.status, 'recoverable');
  assert.equal(recovered.profile.clips.PistolReload.left[0].position[0], 0);
  assert.equal(recovered.updatedAt, 1234);
  assert.throws(() => { recovered.profile.clips = {}; }, TypeError);
  const history = new History(profile(), {revision: 'revision-b'});
  const stale = recoverDraft(serialized, history.savedRevision);
  assert.equal(stale.status, 'stale');
  assert.deepEqual(history.state.clips, {});
  assert.equal(recoverDraft(serialized, null).status, 'stale');
  assert.equal(recoverDraft(serializeDraft(profile(), null), null).status, 'stale');
});

test('malformed, unsupported and incomplete drafts are rejected without throwing', () => {
  assert.equal(recoverDraft(null, 'r').status, 'none');
  assert.equal(recoverDraft('', 'r').status, 'none');
  for (const raw of ['{broken', '{}', 'null', '[]']) assert.equal(recoverDraft(raw, 'r').status, 'invalid');
  const draft = JSON.parse(serializeDraft(profile(), 'r'));
  for (const changed of [{...draft, version: 999}, {...draft, profile: {schema: 2}},
    {...draft, updatedAt: null}, {...draft, savedRevision: undefined}]) {
    assert.equal(recoverDraft(JSON.stringify(changed), 'r').status, 'invalid');
  }
});
