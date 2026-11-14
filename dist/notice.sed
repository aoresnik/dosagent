# Extracts the copyright notice from a Watt-32 source file for NOTICES.TXT,
# without the comment markers. Use with sed -n.
/++Copyright++/,/--Copyright--/{
  /++Copyright++\|--Copyright--/d
  s/^ \* \{0,1\}//
  s/^ \*$//
  /^-$/d
  p
}
