const express = require('express');
const { v4: uuidv4 } = require('uuid');
const db = require('../db');

const router = express.Router();

router.get('/', (req, res) => {
  const events = db.prepare(`
    SELECT e.*, (SELECT COUNT(*) FROM rsvps WHERE event_id = e.id) AS rsvp_count
    FROM events e WHERE e.owner_id = ? ORDER BY e.event_date, e.event_time
  `).all(req.session.user.id);
  res.render('events/list', { events });
});

router.get('/new', (req, res) => res.render('events/form', { event: null }));

router.post('/new', (req, res) => {
  const { title, description, event_date, event_time, location } = req.body;
  const invite_slug = uuidv4().replace(/-/g, '').slice(0, 12);
  db.prepare(`
    INSERT INTO events (owner_id, title, description, event_date, event_time, location, invite_slug)
    VALUES (?, ?, ?, ?, ?, ?, ?)
  `).run(req.session.user.id, title || 'Private Event', description || '', event_date, event_time || '19:00', location || '', invite_slug);
  res.redirect('/events');
});

router.get('/:id', (req, res) => {
  const event = db.prepare('SELECT * FROM events WHERE id = ? AND owner_id = ?').get(req.params.id, req.session.user.id);
  if (!event) return res.status(404).send('Event not found');
  const rsvps = db.prepare(`
    SELECT r.*, u.name, u.email FROM rsvps r JOIN users u ON r.user_id = u.id WHERE r.event_id = ?
  `).all(event.id);
  const baseUrl = (req.protocol + '://' + req.get('host')).replace(/\/$/, '');
  res.render('events/detail', { event, rsvps, fullUrl: baseUrl + '/invite/' + event.invite_slug });
});

router.get('/:id/edit', (req, res) => {
  const event = db.prepare('SELECT * FROM events WHERE id = ? AND owner_id = ?').get(req.params.id, req.session.user.id);
  if (!event) return res.status(404).send('Event not found');
  res.render('events/form', { event });
});

router.post('/:id/edit', (req, res) => {
  const { title, description, event_date, event_time, location } = req.body;
  db.prepare(`
    UPDATE events SET title = ?, description = ?, event_date = ?, event_time = ?, location = ?
    WHERE id = ? AND owner_id = ?
  `).run(title, description, event_date, event_time, location, req.params.id, req.session.user.id);
  res.redirect('/events/' + req.params.id);
});

module.exports = router;
