// Run the actual form handler from both pages, using an in-memory API and DOM.
const assert = require('node:assert/strict');
const {webcrypto} = require('node:crypto');
const {readFileSync} = require('node:fs');
const {join} = require('node:path');
const vm = require('node:vm');
const {test} = require('node:test');

function riderForm(page, loseFirstResponse = false) {
  const html = readFileSync(join(__dirname, '../web', page), 'utf8');
  const section = (start, end) => html.slice(html.indexOf(start), html.indexOf(end));
  const functions = section('function fillRiders(', 'async function loadSettings(');
  const handler = section("el('edit-rider').addEventListener", "el('auto-form').addEventListener");
  const readback = page === 'index.html' ? section('async function checkAfterWrite(', 'async function refresh()')
    + section('async function refreshRiders(', 'function updateBandLink(') : '';
  const controls = new Map();
  let submit;
  const el = id => {
    if (!controls.has(id)) controls.set(id, {
      value: ['rider-select', 'edit-rider', 'rider-sex'].includes(id) ? '0' : '',
      replaceChildren(...options) { this.options = options; this.value = options[0]?.value || ''; },
      addEventListener(event, fn) { if (id === 'rider-form' && event === 'submit') submit = fn; }
    });
    return controls.get(id);
  };
  const people = [];
  const calls = [];
  const created = new Map();
  const response = () => ({riders: structuredClone(people), mode: 'manual', selected_id: 0});
  const context = vm.createContext({
    el, crypto: webcrypto, document: {createElement: () => ({value: '', textContent: ''})},
    riders: [], result() {}, updateBandLink() {},
    async fetchJSON(url) {
      assert.equal(url, '/api/riders');
      return response();
    },
    async post(url, action, fields) {
      assert.equal(url, '/api/riders'); assert.equal(action, 'riders');
      calls.push({...fields});
      const {create_key: key, ...profile} = fields;
      if (created.has(key)) return {...response(), saved_id: created.get(key), already_saved: true};
      const id = Number(profile.id) || people.length + 1;
      const rider = {...profile, id, band_address: ''};
      const index = people.findIndex(r => r.id === id);
      if (index < 0) people.push(rider); else people[index] = rider;
      if (key) created.set(key, id);
      if (loseFirstResponse && calls.length === 1) throw Object.assign(new Error('Lost response'), {uncertain: true});
      return {...response(), saved_id: id};
    }
  });
  vm.runInContext(functions + readback + handler, context);
  const button = {disabled: false};
  return {el, people, calls, button, save: () => submit({preventDefault() {}, submitter: button})};
}

for (const page of ['index.html', 'hr-mock.html']) {
  test(`${page}: create, correct and save keeps one profile`, async () => {
    const {el, people, calls, button, save} = riderForm(page);
    el('rider-name').value = 'New rider';
    if (page === 'index.html') el('rider-ftp').value = '232';
    await save();
    assert.equal(el('edit-rider').value, '1', 'successful create must enter edit mode');
    if (page === 'index.html') {
      assert.equal(calls[0].ftp_w, '232');
      assert.equal(el('rider-ftp').value, '232');
    }
    assert.equal(el('rider-select').value, '0', 'editing does not switch the active rider');
    el('rider-name').value = 'Corrected name';
    await save();
    assert.equal(calls[1].id, '1');
    assert.equal(people.length, 1);
    assert.equal(people[0].name, 'Corrected name');
    assert.equal(button.disabled, false);
  });
}

test('index.html: a lost create response can be retried without duplicating the rider', async () => {
  const {el, people, calls, button, save} = riderForm('index.html', true);
  el('rider-name').value = 'New rider';
  await save();
  assert.equal(people.length, 1);
  assert.equal(el('edit-rider').value, '0');
  assert.equal(button.disabled, false);

  el('rider-name').value = 'Corrected after timeout';
  await save();
  assert.equal(people.length, 1);
  assert.match(calls[0].create_key, /^[0-9a-f]{16}$/);
  assert.equal(calls[1].create_key, calls[0].create_key);
  assert.equal(el('edit-rider').value, '1');
  assert.equal(people[0].name, 'New rider');
  assert.equal(el('rider-name').value, 'Corrected after timeout');

  await save();
  assert.equal(calls[2].id, '1');
  assert.equal(people[0].name, 'Corrected after timeout');
});
