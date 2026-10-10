// Mentions: "@name" in a message calls for that person's attention, and
// "@everyone" for everybody's. Usernames are a-z, 0-9, _ and -.

final _mention = RegExp(r'(^|[^A-Za-z0-9_@-])@([A-Za-z0-9_-]+)');

/// The names mentioned in a message, lower-cased, without the @.
Set<String> mentionedNames(String body) =>
    {for (final m in _mention.allMatches(body)) m.group(2)!.toLowerCase()};

/// Whether a message calls for the attention of the person named [username].
bool mentionsUser(String body, String username) {
  if (username.isEmpty) return false;
  final names = mentionedNames(body);
  return names.contains(username.toLowerCase()) || names.contains('everyone');
}

/// Where the mentions are in a message: (start, end) of each "@name".
List<(int, int)> mentionSpans(String body) => [
      for (final m in _mention.allMatches(body))
        (m.start + m.group(1)!.length, m.end),
    ];
