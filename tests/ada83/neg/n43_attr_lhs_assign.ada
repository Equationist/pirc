-- Pass 4 negative test: X'Length := 1 and "abc" := "def" must not ICE in requtil.a
procedure N43_Bad_Lhs is
   S : String (1 .. 3) := "abc";
begin
   S'Length := 1;
   "abc" := "def";
end N43_Bad_Lhs;
