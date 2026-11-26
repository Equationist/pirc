-- Pass 4 negative test: A'First(-1) and A'First(0) must not ICE in attwalk.a
procedure N45_Neg_Dim is
   A : array (1 .. 5) of Integer;
   X : Integer := A'First (-1);
   Y : Integer := A'First (0);
begin
   null;
end N45_Neg_Dim;
