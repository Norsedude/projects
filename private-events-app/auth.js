function requireLogin(req, res, next) {
  if (!req.session.user) {
    const returnTo = req.originalUrl || '/events';
    return res.redirect('/auth/login?returnTo=' + encodeURIComponent(returnTo));
  }
  next();
}

module.exports = { requireLogin };
