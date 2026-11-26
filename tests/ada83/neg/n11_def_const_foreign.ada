-- Negative test: P3-7.12 deferred constant for foreign private type
procedure Neg_Def_Const_Foreign is
   package P1 is
      type T is private;
   private
      type T is new Integer;
   end P1;
   package P2 is
      C : constant P1.T;
   private
      C : constant P1.T := P1.T'(0);
   end P2;
begin
   null;
end Neg_Def_Const_Foreign;
