void rot13(char *s) {
	for (; *s != 0; s++) {
		if ((*s >= 'A' && *s <= 'M') || (*s >= 'a' && *s <= 'm')) {
			*s = *s + 13;
		} else if ((*s >= 'N' && *s <= 'Z') || (*s >= 'n' && *s <= 'z')) {
			*s = *s - 13;
		}
	}
}
