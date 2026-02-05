const express = require('express');
const db = require('../db');

const router = express.Router();

router.get('/scan', (req, res) => {
  res.render('checkin/scan');
});

router.get('/verify/:token', (req, res) => {
  const rsvp = db.prepare(`
    SELECT r.*, e.title AS event_title, e.event_date, e.event_time, u.name, u.email
    FROM rsvps r JOIN events e ON r.event_id = e.id JOIN users u ON r.user_id = u.id
    WHERE r.qr_token = ?
  `).get(req.params.token);
  const wantsJson = req.get('Accept') && req.get('Accept').includes('application/json');
  if (!rsvp) {
    if (wantsJson) return res.json({ ok: false, message: 'Invalid or already-used QR code.' });
    return res.render('checkin/result', { success: false, message: 'Invalid or already-used QR code.' });
  }
  if (rsvp.status === 'checked_in') {
    if (wantsJson) return res.json({ ok: false, message: 'This pass was already used to check in.' });
    return res.render('checkin/result', { success: false, message: 'This pass was already used to check in.' });
  }
  db.prepare('UPDATE rsvps SET status = ?, checked_in_at = datetime("now") WHERE id = ?').run('checked_in', rsvp.id);
  if (wantsJson) return res.json({ ok: true, name: rsvp.name, event: rsvp.event_title, message: 'Checked in successfully!' });
  res.render('checkin/result', { success: true, name: rsvp.name, event: rsvp.event_title });
});

module.exports = router;
