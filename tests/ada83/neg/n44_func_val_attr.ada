-- Pass 4 negative test: Func'Val(1) must not ICE in attwalk.a
procedure N44_Func_Val is
   function F return Integer is begin return 1; end F;
   X : Integer := F'Val (1);
begin
   null;
end N44_Func_Val;
