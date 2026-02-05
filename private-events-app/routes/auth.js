const express = require('express');
const bcrypt = require('bcryptjs');
const db = require('../db');

const router = express.Router();

router.get('/login', (req, res) => {
  if (req.session.user) return res.redirect(req.query.returnTo || '/events');
  res.render('auth/login', { returnTo: req.query.returnTo || '/events', error: null });
});

router.post('/login', (req, res) => {
  const { email, password } = req.body;
  const returnTo = req.body.returnTo || '/events';
  const user = db.prepare('SELECT * FROM users WHERE email = ?').get(email);
  if (!user || !bcrypt.compareSync(password, user.password_hash)) {
    return res.render('auth/login', { returnTo, error: 'Invalid email or password.' });
  }
  req.session.user = { id: user.id, email: user.email, name: user.name };
  res.redirect(returnTo);
});

router.get('/signup', (req, res) => {
  if (req.session.user) return res.redirect('/events');
  res.render('auth/signup', { error: null });
});

router.post('/signup', (req, res) => {
  const { email, password, name } = req.body;
  if (!email || !password || !name) {
    return res.render('auth/signup', { error: 'All fields are required.' });
  }
  const hash = bcrypt.hashSync(password, 10);
  try {
    db.prepare('INSERT INTO users (email, password_hash, name) VALUES (?, ?, ?)').run(email, hash, name);
  } catch (e) {
    if (e.code === 'SQLITE_CONSTRAINT_UNIQUE')
      return res.render('auth/signup', { error: 'That email is already registered.' });
    throw e;
  }
  const user = db.prepare('SELECT id, email, name FROM users WHERE email = ?').get(email);
  req.session.user = user;
  res.redirect('/events');
});

router.post('/logout', (req, res) => {
  req.session.destroy();
  res.redirect('/auth/login');
});

module.exports = router;
