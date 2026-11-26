-- Pass 4 negative test: ("abc")'Length must not ICE in aggreso.a
procedure N41_Paren_Str_Len is
   N : Integer := ("abc")'Length;
begin
   null;
end N41_Paren_Str_Len;
