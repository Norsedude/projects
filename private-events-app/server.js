const express = require('express');
const session = require('express-session');
const path = require('path');
const QRCode = require('qrcode');
const db = require('./db');
const auth = require('./auth');
const eventsRouter = require('./routes/events');
const authRouter = require('./routes/auth');
const inviteRouter = require('./routes/invite');
const checkinRouter = require('./routes/checkin');

const app = express();
const PORT = process.env.PORT || 3000;

app.set('view engine', 'ejs');
app.set('views', path.join(__dirname, 'views'));

app.use(express.urlencoded({ extended: true }));
app.use(express.json());
app.use(express.static(path.join(__dirname, 'public')));

app.use(session({
  secret: process.env.SESSION_SECRET || 'private-events-secret-change-in-production',
  resave: false,
  saveUninitialized: false,
  cookie: { secure: false, maxAge: 7 * 24 * 60 * 60 * 1000 }
}));

app.use((req, res, next) => {
  res.locals.user = req.session.user || null;
  next();
});

app.use('/auth', authRouter);
app.use('/events', auth.requireLogin, eventsRouter);
app.use('/invite', inviteRouter);
app.use('/check-in', checkinRouter);

app.get('/', (req, res) => {
  if (req.session.user) return res.redirect('/events');
  res.redirect('/auth/login');
});

app.get('/qr/:token', (req, res) => {
  const rsvp = db.prepare('SELECT id FROM rsvps WHERE qr_token = ?').get(req.params.token);
  if (!rsvp) return res.status(404).send('Invalid or expired pass.');
  const baseUrl = (req.protocol + '://' + req.get('host')).replace(/\/$/, '');
  const verifyUrl = baseUrl + '/check-in/verify/' + req.params.token;
  QRCode.toBuffer(verifyUrl, { type: 'png', width: 280, margin: 2 })
    .then((buf) => {
      res.type('png').send(buf);
    })
    .catch(() => res.status(500).send('Error generating QR'));
});

app.listen(PORT, () => {
  console.log(`Private Events app running at http://localhost:${PORT}`);
});
