/// Authenticated user snapshot. Carries just enough to render the
/// dashboard header — no permissions, no preferences, no scopes.
class User {
  const User({
    required this.userId,
    required this.email,
    required this.createdAt,
  });

  final String userId;
  final String email;
  final DateTime createdAt;

  factory User.fromJson(Map<String, dynamic> json) {
    return User(
      userId: json['user_id'] as String,
      email: json['email'] as String,
      createdAt: DateTime.parse(json['created_at'] as String),
    );
  }
}