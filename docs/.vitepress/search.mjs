export function tokenize(text) {
  const tokens = [];
  for (const match of text.toLowerCase().matchAll(/[\p{Script=Han}]+|[\p{L}\p{N}_]+/gu)) {
    const value = match[0];
    if (/\p{Script=Han}/u.test(value)) {
      const characters = Array.from(value);
      tokens.push(...characters);
      for (let i = 0; i + 1 < characters.length; i++) tokens.push(characters[i] + characters[i + 1]);
    } else {
      tokens.push(value);
    }
  }
  return tokens;
}
