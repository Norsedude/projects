const express = require('express');
const { v4: uuidv4 } = require('uuid');
const db = require('../db');

const router = express.Router();

router.get('/:slug', (req, res) => {
  const event = db.prepare('SELECT * FROM events WHERE invite_slug = ?').get(req.params.slug);
  if (!event) return res.status(404).render('error', { message: 'This invite link is invalid or has expired.' });
  if (!req.session.user) {
    return res.redirect('/auth/login?returnTo=' + encodeURIComponent('/invite/' + req.params.slug));
  }
  const existing = db.prepare('SELECT * FROM rsvps WHERE event_id = ? AND user_id = ?').get(event.id, req.session.user.id);
  if (existing) {
    return res.redirect('/invite/' + req.params.slug + '/confirmed');
  }
  res.render('invite/rsvp', { event });
});

router.post('/:slug', (req, res) => {
  const event = db.prepare('SELECT * FROM events WHERE invite_slug = ?').get(req.params.slug);
  if (!event) return res.status(404).send('Invalid invite.');
  if (!req.session.user) return res.redirect('/auth/login?returnTo=' + encodeURIComponent('/invite/' + req.params.slug));
  const qr_token = uuidv4();
  try {
    db.prepare('INSERT INTO rsvps (event_id, user_id, qr_token) VALUES (?, ?, ?)').run(event.id, req.session.user.id, qr_token);
  } catch (e) {
    if (e.code === 'SQLITE_CONSTRAINT_UNIQUE') return res.redirect('/invite/' + req.params.slug + '/confirmed');
    throw e;
  }
  res.redirect('/invite/' + req.params.slug + '/confirmed');
});

router.get('/:slug/confirmed', (req, res) => {
  const event = db.prepare('SELECT * FROM events WHERE invite_slug = ?').get(req.params.slug);
  if (!event) return res.status(404).render('error', { message: 'Invalid invite.' });
  if (!req.session.user) return res.redirect('/auth/login?returnTo=' + encodeURIComponent('/invite/' + req.params.slug));
  const rsvp = db.prepare('SELECT * FROM rsvps WHERE event_id = ? AND user_id = ?').get(event.id, req.session.user.id);
  if (!rsvp) return res.redirect('/invite/' + req.params.slug);
  res.render('invite/confirmed', { event, rsvp });
});

module.exports = router;
