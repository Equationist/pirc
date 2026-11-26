-- Pass 4 negative test: duplicate spec-less subprogram bodies in same declarative part
procedure N51_Dup_Body is
   procedure Foo is
   begin
      null;
   end Foo;
   procedure Foo is
   begin
      null;
   end Foo;
begin
   Foo;
end N51_Dup_Body;
